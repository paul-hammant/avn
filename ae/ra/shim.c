/*
 * Copyright 2026 Paul Hammant (portions).
 * Portions copyright Apache Subversion project contributors (2001-2026).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

/* ra/shim.c — libsvn_ra: REST client to aether-svnserver.
 *
 * Mirrors repos/shim.c's query shape, but over HTTP + JSON. The handles
 * returned here (svnae_ra_log, svnae_ra_list, svnae_ra_info) look and
 * behave the same as their server-side counterparts so Aether callers
 * that already know the repos query API can switch repository sources
 * without reshape.
 *
 * Round 46 moved the HTTP plumbing to Aether (ae/ra/http_client.ae,
 * which uses std.http.client v2 — request builders + headers + status
 * + response-header lookup). The C side here keeps its existing
 * http_get_ex / http_post_json signatures so every downstream caller
 * (handler shims, log/paths/blame builders, commit/copy/branch_create)
 * links unchanged. The bodies are now thin wrappers: build a packed-
 * envelope through the Aether helper, slice status / node-hash / body
 * out, hand them to the existing out-params.
 *
 * Auth state (X-Svnae-User, X-Svnae-Superuser tokens) lives in C —
 * set via svnae_ra_set_user / svnae_ra_set_superuser_token, exposed
 * to the Aether helper through aether_ra_get_user /
 * aether_ra_get_super_token.
 *
 * JSON parse+build lives entirely on the Aether side (ae/ra/parse.ae,
 * ae/ra/commit_build.ae); nothing in this file touches std.json
 * directly.
 */

#include "aether_json.h"    /* JsonValue / json_create_number used by the
                               svnae_ra_json_int helper the commit-build
                               Aether module calls back into. */
#include "aether_string.h"  /* aether_string_data / aether_string_length */
#include "../subr/pin_list.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- HTTP plumbing ---------------------------------------------------- */

/* Per-process auth context — set once at CLI startup, attached to every
 * outgoing request. Placeholder auth: the client claims identity; the
 * server trusts it. Superuser token proves bypass rights via a shared
 * secret. */
static char *g_client_user = NULL;
static char *g_client_super_token = NULL;

/* URL builders ported to Aether (ae/ra/urls.ae). */
extern const char *aether_url_rev_info(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_paths(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_cat(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_list(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_props(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_blame(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_branches_create(const char *base, const char *repo, const char *branch_name);
extern const char *aether_url_info(const char *base, const char *repo);
extern const char *aether_url_log(const char *base, const char *repo);
extern const char *aether_url_commit(const char *base, const char *repo);
extern const char *aether_url_copy(const char *base, const char *repo);
extern const char *aether_url_rev_hashes(const char *base, const char *repo, int rev, const char *path);

void svnae_ra_set_user(const char *user) {
    free(g_client_user);
    g_client_user = user && *user ? strdup(user) : NULL;
}
void svnae_ra_set_superuser_token(const char *token) {
    free(g_client_super_token);
    g_client_super_token = token && *token ? strdup(token) : NULL;
}

/* Auth getters called from ae/ra/http_client.ae's send_with_auth.
 * Empty string when the corresponding state isn't set — the Aether
 * side checks string.length() before adding the header. */
const char *aether_ra_get_user(void) {
    return g_client_user ? g_client_user : "";
}
const char *aether_ra_get_super_token(void) {
    return g_client_super_token ? g_client_super_token : "";
}

/* Environment-variable accessor moved to std.os::getenv;
 * svnae_http_get_body lives below, after http_get is defined. */

/* The libcurl-using helpers (build_auth_headers, buf_write_cb,
 * hdr_capture_cb, struct buf, struct hdr_capture) moved to
 * ae/ra/http_client.ae in round 46. The Aether helper returns a
 * std.http.client response handle (ptr) the C wrapper reads via
 * typed accessors — status, body, header — and then frees. */

extern const void *aether_ra_http_get(const char *url);
extern const void *aether_ra_http_post_json(const char *url, const char *body, int body_len);
extern int         aether_ra_http_response_status(const void *resp);
extern const char *aether_ra_http_response_body  (const void *resp);
extern const char *aether_ra_http_response_header(const void *resp, const char *name);
extern void        aether_ra_http_response_free  (const void *resp);

/* Copy an AetherString-backed body (length-aware, embedded-NUL safe)
 * into a fresh malloc'd buffer that out-params expect. */
static int
take_body(const char *src, char **out_body, size_t *out_len)
{
    const char *data = aether_string_data(src);
    int n = (int)aether_string_length(src);
    char *b = malloc((size_t)n + 1);
    if (!b) return -1;
    if (n > 0) memcpy(b, data, (size_t)n);
    b[n] = '\0';
    *out_body = b;
    *out_len = (size_t)n;
    return 0;
}

/* Same for a header value — node-hash etc. Returns NULL when the
 * source is empty (header was absent), matching the original libcurl
 * shim's NULL-on-absent contract. */
static char *
take_header(const char *src)
{
    int n = (int)aether_string_length(src);
    if (n == 0) return NULL;
    const char *data = aether_string_data(src);
    char *h = malloc((size_t)n + 1);
    if (!h) return NULL;
    memcpy(h, data, (size_t)n);
    h[n] = '\0';
    return h;
}

/* Perform GET. Returns 0 on success, -1 on transport failure. On
 * success body is malloc'd NUL-terminated; caller frees.
 * `out_node_hash` (nullable): if non-NULL, captures the
 * X-Svnae-Node-Hash response header as a new malloc'd string the
 * caller must free. NULL when absent.
 *
 * Non-static so ae/client/verify_shim.c can call it without adding a
 * fourth RA accessor for every endpoint. */
int
http_get_ex(const char *url, char **out_body, size_t *out_len, int *out_status,
            char **out_node_hash)
{
    const void *resp = aether_ra_http_get(url);
    if (!resp) return -1;

    *out_status = aether_ra_http_response_status(resp);

    const char *body_str = aether_ra_http_response_body(resp);
    if (take_body(body_str, out_body, out_len) != 0) {
        aether_ra_http_response_free(resp);
        return -1;
    }

    if (out_node_hash) {
        const char *hash_str = aether_ra_http_response_header(resp, "X-Svnae-Node-Hash");
        *out_node_hash = take_header(hash_str);
    }

    aether_ra_http_response_free(resp);
    return 0;
}

/* Back-compat thin wrapper. */
static int
http_get(const char *url, char **out_body, size_t *out_len, int *out_status)
{
    return http_get_ex(url, out_body, out_len, out_status, NULL);
}

/* Simple "fetch URL and return the body as a malloc'd string" used by
 * CLI subcommands that just want to print a JSON response verbatim.
 * Returns "" on any failure. Caller takes ownership. */
char *svnae_http_get_body(const char *url) {
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return strdup("");
    if (status != 200) { free(body); return strdup(""); }
    return body ? body : strdup("");
}

/* GET `url`, return malloc'd body iff transport succeeded AND status
 * is 200. Returns NULL on transport failure or any non-200 status —
 * the caller can't tell those apart, but every existing caller
 * already collapsed both into "give up" anyway.
 *
 * `out_len` (nullable) receives the body byte count on success. */
static char *
http_get_200(const char *url, size_t *out_len)
{
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }
    if (out_len) *out_len = len;
    return body;
}

static int
http_post_json(const char *url, const char *body, char **out_resp, size_t *out_len, int *out_status)
{
    const void *resp = aether_ra_http_post_json(url, body, (int)strlen(body));
    if (!resp) return -1;

    *out_status = aether_ra_http_response_status(resp);

    const char *body_str = aether_ra_http_response_body(resp);
    int rc = take_body(body_str, out_resp, out_len);
    aether_ra_http_response_free(resp);
    return rc;
}

/* ---- public API ------------------------------------------------------ */

/* head_rev: returns the current revision number, or -1 on any failure. */
extern int aether_ra_parse_head_rev(const char *body);
extern int aether_ra_parse_rev_response(const char *body);
int
svnae_ra_head_rev(const char *base_url, const char *repo_name)
{
    char *body = http_get_200(aether_url_info(base_url, repo_name), NULL);
    if (!body) return -1;
    /* JSON parse + head-field extraction ported to
     * ae/ra/parse.ae::ra_parse_head_rev. */
    int rev = aether_ra_parse_head_rev(body);
    free(body);
    return rev;
}

/* Query the server's primary content-address algorithm. Returns a
 * malloc'd string (caller frees via free()) or NULL on failure. If
 * the server predates Phase 6.1 and omits the field, returns "sha1"
 * so callers can safely default. */
extern const char *aether_ra_parse_hash_algo(const char *body);
char *
svnae_ra_hash_algo(const char *base_url, const char *repo_name)
{
    char *body = http_get_200(aether_url_info(base_url, repo_name), NULL);
    if (!body) return NULL;
    /* JSON parse ported to ae/ra/parse.ae::ra_parse_hash_algo.
     * Parser returns "" only on parse failure; defaults to "sha1"
     * when the field is absent (pre-Phase-6.1 servers). */
    const char *algo = aether_ra_parse_hash_algo(body);
    free(body);
    if (!algo || !*algo) return NULL;
    return strdup(algo);
}

/* ---- packed-record accessor glue ------------------------------------ *
 *
 * ae/ra/parse.ae produces packed "<N>\x02<entry>\x02<entry>..." strings
 * where each entry is a "<f0>\x01<f1>\x01..." record. The per-field
 * accessors (log/paths/blame/list) used to re-parse that packed form
 * in C, rebuilding a struct-of-arrays. ae/ra/packed.ae now exposes
 * typed accessors over the packed string directly, so each handle
 * here just owns the packed payload plus a per-handle "pin list"
 * that keeps returned string copies alive until the handle is freed.
 *
 * The pin list exists because callers routinely hold one accessor's
 * return value across another accessor call (or across a recursive
 * call that hits the same accessor). A per-accessor TLS slot would
 * clobber them; strdup-and-pin-to-handle gives the stable-pointer
 * contract the C API has always had. */


/* ---- shared list-handle internals -----------------------------------
 *
 * The log / paths / blame / list handles all share the exact same
 * shape: own one packed-string payload (parsed Aether-side), remember
 * the row count, and pin per-accessor strdup'd copies so callers can
 * hold pointers across calls.
 *
 * Round 45 collapsed four ~60-line builder/getter blocks into one
 * shared internal struct + ra_handle_from_url + ra_handle_free, with
 * thin per-domain wrappers preserving the public symbols.
 */

struct svnae_ra_handle { char *packed; int n; struct pin_list pins; };

typedef const char *(*ra_parse_fn)(const char *body);
typedef int         (*ra_count_fn)(const char *packed);

static struct svnae_ra_handle *
ra_handle_from_url(const char *url, ra_parse_fn parse, ra_count_fn count)
{
    char *body = http_get_200(url, NULL);
    if (!body) return NULL;

    const char *packed = parse(body);
    free(body);
    if (!packed || !*packed) return NULL;

    struct svnae_ra_handle *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->packed = strdup(packed);
    h->n = count ? count(h->packed) : 0;
    return h;
}

static void
ra_handle_free(struct svnae_ra_handle *h)
{
    if (!h) return;
    free(h->packed);
    pin_list_free(&h->pins);
    free(h);
}

/* ---- log handle ------------------------------------------------------ */

extern const char *aether_ra_parse_log(const char *body);
extern int         aether_ra_log_count(const char *packed);
extern int         aether_ra_log_rev(const char *packed, int i);
extern const char *aether_ra_log_author(const char *packed, int i);
extern const char *aether_ra_log_date(const char *packed, int i);
extern const char *aether_ra_log_msg(const char *packed, int i);

struct svnae_ra_log *
svnae_ra_log(const char *base_url, const char *repo_name)
{
    return (struct svnae_ra_log *)ra_handle_from_url(
        aether_url_log(base_url, repo_name),
        aether_ra_parse_log, aether_ra_log_count);
}

int svnae_ra_log_count(const struct svnae_ra_log *lg)
{
    const struct svnae_ra_handle *h = (const struct svnae_ra_handle *)lg;
    return h ? h->n : 0;
}

int
svnae_ra_log_rev(const struct svnae_ra_log *lg, int i)
{
    const struct svnae_ra_handle *h = (const struct svnae_ra_handle *)lg;
    if (!h || i < 0 || i >= h->n) return -1;
    return aether_ra_log_rev(h->packed, i);
}

const char *
svnae_ra_log_author(struct svnae_ra_log *lg, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)lg;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_log_author(h->packed, i));
}

const char *
svnae_ra_log_date(struct svnae_ra_log *lg, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)lg;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_log_date(h->packed, i));
}

const char *
svnae_ra_log_msg(struct svnae_ra_log *lg, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)lg;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_log_msg(h->packed, i));
}

void svnae_ra_log_free(struct svnae_ra_log *lg)
{
    ra_handle_free((struct svnae_ra_handle *)lg);
}

/* ---- paths-changed handle (for `svn log --verbose`) ----------------- */

extern const char *aether_ra_parse_paths(const char *body);
extern int         aether_ra_paths_count(const char *packed);
extern const char *aether_ra_paths_action(const char *packed, int i);
extern const char *aether_ra_paths_path(const char *packed, int i);

struct svnae_ra_paths *
svnae_ra_paths_changed(const char *base_url, const char *repo_name, int rev)
{
    return (struct svnae_ra_paths *)ra_handle_from_url(
        aether_url_rev_paths(base_url, repo_name, rev),
        aether_ra_parse_paths, aether_ra_paths_count);
}

int svnae_ra_paths_count(const struct svnae_ra_paths *P)
{
    const struct svnae_ra_handle *h = (const struct svnae_ra_handle *)P;
    return h ? h->n : 0;
}

const char *
svnae_ra_paths_action(struct svnae_ra_paths *P, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)P;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_paths_action(h->packed, i));
}

const char *
svnae_ra_paths_path(struct svnae_ra_paths *P, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)P;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_paths_path(h->packed, i));
}

void svnae_ra_paths_free(struct svnae_ra_paths *P)
{
    ra_handle_free((struct svnae_ra_handle *)P);
}

/* ---- blame handle (Phase 7.6) --------------------------------------- */

extern const char *aether_ra_parse_blame(const char *body);
extern int         aether_ra_blame_count(const char *packed);
extern int         aether_ra_blame_rev(const char *packed, int i);
extern const char *aether_ra_blame_author(const char *packed, int i);
extern const char *aether_ra_blame_text(const char *packed, int i);

struct svnae_ra_blame *
svnae_ra_blame(const char *base_url, const char *repo_name,
              int rev, const char *path)
{
    return (struct svnae_ra_blame *)ra_handle_from_url(
        aether_url_rev_blame(base_url, repo_name, rev, path),
        aether_ra_parse_blame, aether_ra_blame_count);
}

int svnae_ra_blame_count(const struct svnae_ra_blame *B)
{
    const struct svnae_ra_handle *h = (const struct svnae_ra_handle *)B;
    return h ? h->n : 0;
}

int
svnae_ra_blame_rev(const struct svnae_ra_blame *B, int i)
{
    const struct svnae_ra_handle *h = (const struct svnae_ra_handle *)B;
    if (!h || i < 0 || i >= h->n) return -1;
    return aether_ra_blame_rev(h->packed, i);
}

const char *
svnae_ra_blame_author(struct svnae_ra_blame *B, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)B;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_blame_author(h->packed, i));
}

const char *
svnae_ra_blame_text(struct svnae_ra_blame *B, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)B;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_blame_text(h->packed, i));
}

void svnae_ra_blame_free(struct svnae_ra_blame *B)
{
    ra_handle_free((struct svnae_ra_handle *)B);
}

/* ---- info handle ----------------------------------------------------- */

struct svnae_ra_info { char *packed; struct pin_list pins; };

extern const char *aether_ra_parse_info_rev(const char *body);
extern int         aether_ra_info_rev(const char *packed);
extern const char *aether_ra_info_author(const char *packed);
extern const char *aether_ra_info_date(const char *packed);
extern const char *aether_ra_info_msg(const char *packed);
extern const char *aether_ra_info_root(const char *packed);

struct svnae_ra_info *
svnae_ra_info_rev(const char *base_url, const char *repo_name, int rev)
{
    char *body = http_get_200(aether_url_rev_info(base_url, repo_name, rev), NULL);
    if (!body) return NULL;

    const char *packed = aether_ra_parse_info_rev(body);
    free(body);
    if (!packed || !*packed) return NULL;

    struct svnae_ra_info *I = calloc(1, sizeof *I);
    I->packed = strdup(packed);
    return I;
}

int         svnae_ra_info_rev_num(const struct svnae_ra_info *I) {
    return I ? aether_ra_info_rev(I->packed) : -1;
}
const char *svnae_ra_info_author (struct svnae_ra_info *I) {
    if (!I) return "";
    return pin_str(&I->pins, aether_ra_info_author(I->packed));
}
const char *svnae_ra_info_date   (struct svnae_ra_info *I) {
    if (!I) return "";
    return pin_str(&I->pins, aether_ra_info_date(I->packed));
}
const char *svnae_ra_info_msg    (struct svnae_ra_info *I) {
    if (!I) return "";
    return pin_str(&I->pins, aether_ra_info_msg(I->packed));
}
const char *svnae_ra_info_root   (struct svnae_ra_info *I) {
    if (!I) return "";
    return pin_str(&I->pins, aether_ra_info_root(I->packed));
}

void
svnae_ra_info_free(struct svnae_ra_info *I)
{
    if (!I) return;
    free(I->packed);
    pin_list_free(&I->pins);
    free(I);
}

/* ---- cat ------------------------------------------------------------- *
 *
 * Returns a malloc'd NUL-terminated buffer with the file bytes (body
 * length may be larger than strlen for binary blobs, but for the port's
 * current scope — text + ASCII-ish — strlen suffices. A length-aware
 * variant will land when binary blobs flow end-to-end).
 * Caller frees with svnae_ra_free.
 */

char *
svnae_ra_cat(const char *base_url, const char *repo_name, int rev, const char *path)
{
    /* Skip leading '/' in the user path so URLs look clean. */
    while (*path == '/') path++;
    return http_get_200(aether_url_rev_cat(base_url, repo_name, rev, path), NULL);
}

void svnae_ra_free(char *p) { free(p); }

/* --- remote properties ---------------------------------------------- *
 *
 * GET /repos/{r}/rev/{n}/props/<path> returns a JSON {k:v,...} object.
 * We expose it as a handle with indexed name/value accessors.
 */

struct svnae_ra_props { char *packed; int n; struct pin_list pins; };

extern const char *aether_ra_parse_props(const char *body);
extern int         aether_ra_props_count(const char *packed);
extern const char *aether_ra_props_name(const char *packed, int i);
extern const char *aether_ra_props_value(const char *packed, int i);

struct svnae_ra_props *
svnae_ra_get_props(const char *base_url, const char *repo_name,
                   int rev, const char *path)
{
    while (*path == '/') path++;
    char *body = http_get_200(
        aether_url_rev_props(base_url, repo_name, rev, path), NULL);
    if (!body) return NULL;

    const char *packed = aether_ra_parse_props(body);
    free(body);
    if (!packed || !*packed) return NULL;

    struct svnae_ra_props *P = calloc(1, sizeof *P);
    P->packed = strdup(packed);
    P->n = aether_ra_props_count(P->packed);
    return P;
}

int svnae_ra_props_count(const struct svnae_ra_props *P) { return P ? P->n : 0; }

const char *
svnae_ra_props_name(struct svnae_ra_props *P, int i)
{
    if (!P || i < 0 || i >= P->n) return "";
    return pin_str(&P->pins, aether_ra_props_name(P->packed, i));
}

const char *
svnae_ra_props_value(struct svnae_ra_props *P, int i)
{
    if (!P || i < 0 || i >= P->n) return "";
    return pin_str(&P->pins, aether_ra_props_value(P->packed, i));
}

void
svnae_ra_props_free(struct svnae_ra_props *P)
{
    if (!P) return;
    free(P->packed);
    pin_list_free(&P->pins);
    free(P);
}

/* --- server-side copy ------------------------------------------------ *
 *
 * POST /repos/{r}/copy with { base_rev, from_path, to_path, author, log }.
 * Returns the new revision number, or -1.
 */
extern const char *aether_ra_copy_build_body(int base_rev,
                                              const char *from_path,
                                              const char *to_path,
                                              const char *author,
                                              const char *logmsg);
int
svnae_ra_server_copy(const char *base_url, const char *repo_name,
                     int base_rev,
                     const char *from_path, const char *to_path,
                     const char *author, const char *logmsg)
{
    /* Body serialisation ported to ae/ra/commit_build.ae::ra_copy_build_body. */
    const char *json = aether_ra_copy_build_body(base_rev, from_path, to_path,
                                                 author, logmsg);
    const char *url = aether_url_copy(base_url, repo_name);
    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, json, &resp, &len, &status);
    (void)len;

    int new_rev = -1;
    if (rc == 0 && status == 200 && resp) {
        new_rev = aether_ra_parse_rev_response(resp);
    }
    free(resp);
    return new_rev;
}

/* ---- branch create (Phase 8.2a) ------------------------------------- *
 *
 * POST /repos/{r}/branches/<NAME>/create with JSON
 *   { "base": "main", "include": ["src/**", "README.md"] }
 * Super-user only. Returns the new rev number, or -1 on failure.
 *
 * `includes_joined` is a single newline-separated string for ergonomic
 * passage from Aether (Aether ptr-array types are awkward). We split
 * here and pass as a JSON array. */
extern const char *aether_ra_branch_create_build_body(const char *base,
                                                       const char *includes_joined);
int
svnae_ra_branch_create(const char *base_url, const char *repo_name,
                      const char *name, const char *base,
                      const char *includes_joined)
{
    /* Body serialisation ported to
     * ae/ra/commit_build.ae::ra_branch_create_build_body. */
    const char *json = aether_ra_branch_create_build_body(base, includes_joined ? includes_joined : "");
    const char *url = aether_url_branches_create(base_url, repo_name, name);
    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, json, &resp, &len, &status);
    (void)len;

    int new_rev = -1;
    if (rc == 0 && status == 201 && resp) {
        new_rev = aether_ra_parse_rev_response(resp);
    }
    free(resp);
    return new_rev;
}

/* ---- list ------------------------------------------------------------ */

extern const char *aether_ra_parse_list(const char *body);
extern int         aether_ra_list_count(const char *packed);
extern const char *aether_ra_list_name(const char *packed, int i);
extern const char *aether_ra_list_kind(const char *packed, int i);

struct svnae_ra_list *
svnae_ra_list(const char *base_url, const char *repo_name, int rev, const char *path)
{
    while (*path == '/') path++;
    return (struct svnae_ra_list *)ra_handle_from_url(
        aether_url_rev_list(base_url, repo_name, rev, path),
        aether_ra_parse_list, aether_ra_list_count);
}

int svnae_ra_list_count(const struct svnae_ra_list *L)
{
    const struct svnae_ra_handle *h = (const struct svnae_ra_handle *)L;
    return h ? h->n : 0;
}

const char *
svnae_ra_list_name(struct svnae_ra_list *L, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)L;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_list_name(h->packed, i));
}

const char *
svnae_ra_list_kind(struct svnae_ra_list *L, int i)
{
    struct svnae_ra_handle *h = (struct svnae_ra_handle *)L;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, aether_ra_list_kind(h->packed, i));
}

void svnae_ra_list_free(struct svnae_ra_list *L)
{
    ra_handle_free((struct svnae_ra_handle *)L);
}

/* ---- commit ---------------------------------------------------------- *
 *
 * Aether can't build arrays across FFI easily, so we expose a builder:
 *   cb = ra_commit_begin(base_rev, author, log)
 *   ra_commit_add_file(cb, path, content, len)
 *   ra_commit_mkdir(cb, path)
 *   ra_commit_delete(cb, path)
 *   new_rev = ra_commit_finish(cb, base_url, repo_name)
 *
 * The builder accumulates edits in-memory and only serialises + POSTs
 * at finish time.
 */

struct commit_edit { int op; char *path; unsigned char *content; int content_len; };
/* op: 1=add-file, 2=mkdir, 3=delete. Matches txn_shim.c's edit kinds. */

/* Per-path props collected for commit. Parallel arrays keyed by
 * composite "path\0key" — we flatten for simplicity. When commit_finish
 * serialises, we group by path into nested objects. */
struct commit_prop { char *path; char *key; char *value; };

/* Per-path ACL rules collected for commit. Flat list of (path, rule)
 * like "+alice" / "-eve"; commit_finish groups into array-per-path. */
struct commit_acl { char *path; char *rule; };

struct svnae_ra_commit {
    int   base_rev;
    char *author;
    char *logmsg;
    struct commit_edit *edits;
    int   n;
    int   cap;
    struct commit_prop *props;
    int   n_props;
    int   cap_props;
    struct commit_acl *acls;
    int   n_acls;
    int   cap_acls;
};

struct svnae_ra_commit *
svnae_ra_commit_begin(int base_rev, const char *author, const char *logmsg)
{
    struct svnae_ra_commit *cb = calloc(1, sizeof *cb);
    if (!cb) return NULL;
    cb->base_rev = base_rev;
    cb->author = strdup(author ? author : "");
    cb->logmsg = strdup(logmsg ? logmsg : "");
    return cb;
}

static int
cb_push(struct svnae_ra_commit *cb)
{
    if (cb->n == cb->cap) {
        int nc = cb->cap ? cb->cap * 2 : 8;
        struct commit_edit *p = realloc(cb->edits, (size_t)nc * sizeof *p);
        if (!p) return -1;
        cb->edits = p;
        cb->cap = nc;
    }
    return cb->n++;
}

int
svnae_ra_commit_add_file(struct svnae_ra_commit *cb, const char *path,
                         const char *content, int len)
{
    if (!cb) return -1;
    int i = cb_push(cb);
    if (i < 0) return -1;
    cb->edits[i].op = 1;
    cb->edits[i].path = strdup(path);
    if (len > 0 && content) {
        cb->edits[i].content = malloc((size_t)len);
        memcpy(cb->edits[i].content, content, (size_t)len);
        cb->edits[i].content_len = len;
    } else {
        cb->edits[i].content = NULL;
        cb->edits[i].content_len = 0;
    }
    return 0;
}

int
svnae_ra_commit_mkdir(struct svnae_ra_commit *cb, const char *path)
{
    if (!cb) return -1;
    int i = cb_push(cb);
    if (i < 0) return -1;
    cb->edits[i].op = 2;
    cb->edits[i].path = strdup(path);
    cb->edits[i].content = NULL;
    cb->edits[i].content_len = 0;
    return 0;
}

int
svnae_ra_commit_delete(struct svnae_ra_commit *cb, const char *path)
{
    if (!cb) return -1;
    int i = cb_push(cb);
    if (i < 0) return -1;
    cb->edits[i].op = 3;
    cb->edits[i].path = strdup(path);
    cb->edits[i].content = NULL;
    cb->edits[i].content_len = 0;
    return 0;
}

/* Record an ACL rule for `path` on the pending commit. Multiple calls
 * with the same path accumulate into an ACL array on the wire. Clearing
 * a path's ACL is done by calling svnae_ra_commit_acl_clear, which
 * sends an empty array. */
int
svnae_ra_commit_acl_add(struct svnae_ra_commit *cb,
                        const char *path, const char *rule)
{
    if (!cb) return -1;
    if (cb->n_acls == cb->cap_acls) {
        int nc = cb->cap_acls ? cb->cap_acls * 2 : 8;
        struct commit_acl *p = realloc(cb->acls, (size_t)nc * sizeof *p);
        if (!p) return -1;
        cb->acls = p;
        cb->cap_acls = nc;
    }
    cb->acls[cb->n_acls].path = strdup(path);
    cb->acls[cb->n_acls].rule = strdup(rule);
    cb->n_acls++;
    return 0;
}

/* Record a property to persist on the next commit. The commit payload
 * carries ALL properties for each path the client wants to persist —
 * unmentioned paths inherit the previous revision's props. Call once
 * per (path, key, value). */
int
svnae_ra_commit_set_prop(struct svnae_ra_commit *cb,
                         const char *path, const char *key, const char *value)
{
    if (!cb) return -1;
    if (cb->n_props == cb->cap_props) {
        int nc = cb->cap_props ? cb->cap_props * 2 : 8;
        struct commit_prop *p = realloc(cb->props, (size_t)nc * sizeof *p);
        if (!p) return -1;
        cb->props = p;
        cb->cap_props = nc;
    }
    cb->props[cb->n_props].path  = strdup(path);
    cb->props[cb->n_props].key   = strdup(key);
    cb->props[cb->n_props].value = strdup(value);
    cb->n_props++;
    return 0;
}

/* Consolidated in ae/ffi/openssl/shim.c. */
extern char *svnae_openssl_b64_encode(const unsigned char *src, int len);
#define b64_encode svnae_openssl_b64_encode

/* Commit: serialise edits into JSON body, POST to the server, parse the
 * response. Returns the new revision number or -1 on any failure. Frees
 * the builder on the way out. */
/* --- Aether-callable accessors for struct svnae_ra_commit ---------- *
 *
 * The JSON body serialisation happens in Aether (ae/ra/commit_build.ae)
 * via std.json; these accessors let it walk the in-memory builder
 * without duplicating the struct layout across the boundary. */
int         svnae_ra_cb_base_rev(const struct svnae_ra_commit *cb) {
    return cb ? cb->base_rev : 0;
}
const char *svnae_ra_cb_author(const struct svnae_ra_commit *cb) {
    return (cb && cb->author) ? cb->author : "";
}
const char *svnae_ra_cb_logmsg(const struct svnae_ra_commit *cb) {
    return (cb && cb->logmsg) ? cb->logmsg : "";
}
int         svnae_ra_cb_edit_count(const struct svnae_ra_commit *cb) {
    return cb ? cb->n : 0;
}
int         svnae_ra_cb_edit_op(const struct svnae_ra_commit *cb, int i) {
    return (cb && i >= 0 && i < cb->n) ? cb->edits[i].op : 0;
}
const char *svnae_ra_cb_edit_path(const struct svnae_ra_commit *cb, int i) {
    return (cb && i >= 0 && i < cb->n && cb->edits[i].path) ? cb->edits[i].path : "";
}
/* For add-file edits: base64-encode the content bytes into a TLS
 * buffer and return. Aether doesn't safely carry binary bytes across
 * FFI, so encoding stays on the C side. */
const char *svnae_ra_cb_edit_content_b64(const struct svnae_ra_commit *cb, int i) {
    static __thread char *last = NULL;
    free(last); last = NULL;
    if (!cb || i < 0 || i >= cb->n || cb->edits[i].op != 1) return "";
    last = b64_encode(cb->edits[i].content, cb->edits[i].content_len);
    return last ? last : "";
}
int         svnae_ra_cb_prop_count(const struct svnae_ra_commit *cb) {
    return cb ? cb->n_props : 0;
}
const char *svnae_ra_cb_prop_path(const struct svnae_ra_commit *cb, int i) {
    return (cb && i >= 0 && i < cb->n_props && cb->props[i].path) ? cb->props[i].path : "";
}
const char *svnae_ra_cb_prop_key(const struct svnae_ra_commit *cb, int i) {
    return (cb && i >= 0 && i < cb->n_props && cb->props[i].key) ? cb->props[i].key : "";
}
const char *svnae_ra_cb_prop_value(const struct svnae_ra_commit *cb, int i) {
    return (cb && i >= 0 && i < cb->n_props && cb->props[i].value) ? cb->props[i].value : "";
}
int         svnae_ra_cb_acl_count(const struct svnae_ra_commit *cb) {
    return cb ? cb->n_acls : 0;
}
const char *svnae_ra_cb_acl_path(const struct svnae_ra_commit *cb, int i) {
    return (cb && i >= 0 && i < cb->n_acls && cb->acls[i].path) ? cb->acls[i].path : "";
}
const char *svnae_ra_cb_acl_rule(const struct svnae_ra_commit *cb, int i) {
    return (cb && i >= 0 && i < cb->n_acls && cb->acls[i].rule) ? cb->acls[i].rule : "";
}

/* Aether's std.json.create_number takes a float; this tiny shim
 * lets the commit-builder emit an int rev number without int-to-
 * float conversion syntax on the Aether side. json_create_number
 * is already forward-declared at top of file via cJSON typedefs. */
void *svnae_ra_json_int(int v) { return json_create_number((double)v); }

/* Aether builds the JSON body string; C side takes over for HTTP POST
 * + response parse + builder free. Split this way because http_post_json
 * + libcurl + the TLS-aware resp buffer all live in C. */
extern const char *aether_ra_commit_build_body(const struct svnae_ra_commit *cb);

int
svnae_ra_commit_finish(struct svnae_ra_commit *cb,
                      const char *base_url, const char *repo_name)
{
    if (!cb) return -1;

    const char *body_json = aether_ra_commit_build_body(cb);
    const char *url = aether_url_commit(base_url, repo_name);

    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, body_json, &resp, &len, &status);
    (void)len;

    int new_rev = -1;
    if (rc == 0 && status == 200 && resp) {
        new_rev = aether_ra_parse_rev_response(resp);
    }
    free(resp);

    /* Free the builder's contents. */
    free(cb->author);
    free(cb->logmsg);
    for (int i = 0; i < cb->n; i++) {
        free(cb->edits[i].path);
        free(cb->edits[i].content);
    }
    for (int i = 0; i < cb->n_props; i++) {
        free(cb->props[i].path);
        free(cb->props[i].key);
        free(cb->props[i].value);
    }
    for (int i = 0; i < cb->n_acls; i++) {
        free(cb->acls[i].path);
        free(cb->acls[i].rule);
    }
    free(cb->edits);
    free(cb->props);
    free(cb->acls);
    free(cb);

    return new_rev;
}

/* ---- verify glue (round 51) -----------------------------------------
 *
 * The Merkle-verify walk used to live in ~410 lines of C below this
 * point: verify_file / verify_dir / verify_secondaries_in_dir + their
 * recursion plumbing. The walk is now in ae/ra/verify.ae using
 * std.cryptography v2 + std.http.client v2 + the existing Aether-side
 * URL/list/secondaries parsers. The two public symbols
 * (svnae_client_verify, svnae_client_verify_full) are now Aether
 * exports.
 *
 * The C side keeps two small struct allocators the Aether walk calls
 * back into:
 *   - svnae_verify_counter:  (files, secondaries) accumulator passed
 *     through the recursive secondaries pass.
 *   - svnae_verify_entries:  (name, kind_c, sha) tuples for one dir,
 *     sortable by name. Aether-side dir verification builds one,
 *     populates it during the recursive descent, sorts, then re-emits
 *     the canonical "<kind> <sha> <name>\n" lines for re-hashing.
 *
 * Storage in C because Aether can't allocate a struct-of-arrays with
 * ergonomic field access from FFI. Both helpers are tiny: ~70 lines
 * total. Same shape as svnae_wc_nodelist (round 40).
 */

struct svnae_verify_counter { int files; int secondaries; };

void *svnae_verify_counter_new(void) {
    struct svnae_verify_counter *c = calloc(1, sizeof *c);
    return c;
}
int  svnae_verify_counter_files(const void *p) {
    return p ? ((const struct svnae_verify_counter *)p)->files : 0;
}
int  svnae_verify_counter_secondaries(const void *p) {
    return p ? ((const struct svnae_verify_counter *)p)->secondaries : 0;
}
void svnae_verify_counter_inc_files(void *p) {
    if (p) ((struct svnae_verify_counter *)p)->files++;
}
void svnae_verify_counter_inc_secondaries(void *p) {
    if (p) ((struct svnae_verify_counter *)p)->secondaries++;
}
void svnae_verify_counter_free(void *p) { free(p); }

struct ventry { char *name; int kind_c; char *sha; };
struct svnae_verify_entries { struct ventry *items; int n; int cap; };

void *svnae_verify_entries_new(void) {
    return calloc(1, sizeof(struct svnae_verify_entries));
}
void svnae_verify_entries_add(void *p, const char *name, int kind_c) {
    struct svnae_verify_entries *e = p;
    if (!e) return;
    if (e->n == e->cap) {
        int nc = e->cap ? e->cap * 2 : 8;
        struct ventry *q = realloc(e->items, (size_t)nc * sizeof *q);
        if (!q) return;
        e->items = q; e->cap = nc;
    }
    e->items[e->n].name   = strdup(name ? name : "");
    e->items[e->n].kind_c = kind_c;
    e->items[e->n].sha    = NULL;
    e->n++;
}
void svnae_verify_entries_set_sha(void *p, int i, const char *sha) {
    struct svnae_verify_entries *e = p;
    if (!e || i < 0 || i >= e->n) return;
    free(e->items[i].sha);
    e->items[i].sha = strdup(sha ? sha : "");
}
int svnae_verify_entries_count(const void *p) {
    const struct svnae_verify_entries *e = p;
    return e ? e->n : 0;
}
const char *svnae_verify_entries_name(const void *p, int i) {
    const struct svnae_verify_entries *e = p;
    return (e && i >= 0 && i < e->n && e->items[i].name) ? e->items[i].name : "";
}
int svnae_verify_entries_kind(const void *p, int i) {
    const struct svnae_verify_entries *e = p;
    return (e && i >= 0 && i < e->n) ? e->items[i].kind_c : 102;
}
const char *svnae_verify_entries_sha(const void *p, int i) {
    const struct svnae_verify_entries *e = p;
    return (e && i >= 0 && i < e->n && e->items[i].sha) ? e->items[i].sha : "";
}

static int ventry_cmp(const void *a, const void *b) {
    const struct ventry *ea = a, *eb = b;
    return strcmp(ea->name, eb->name);
}
void svnae_verify_entries_sort(void *p) {
    struct svnae_verify_entries *e = p;
    if (!e || e->n < 2) return;
    qsort(e->items, (size_t)e->n, sizeof *e->items, ventry_cmp);
}
void svnae_verify_entries_free(void *p) {
    struct svnae_verify_entries *e = p;
    if (!e) return;
    for (int i = 0; i < e->n; i++) {
        free(e->items[i].name);
        free(e->items[i].sha);
    }
    free(e->items);
    free(e);
}
