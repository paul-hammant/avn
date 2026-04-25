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
    const char *url = aether_url_info(base_url, repo_name);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return -1;
    if (status != 200) { free(body); return -1; }
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
    const char *url = aether_url_info(base_url, repo_name);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }
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
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

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
    const char *url = aether_url_rev_info(base_url, repo_name, rev);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

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
    const char *url = aether_url_rev_cat(base_url, repo_name, rev, path);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }
    return body;  /* caller owns */
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
    const char *url = aether_url_rev_props(base_url, repo_name, rev, path);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

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
/* ae/client/verify_shim.c — Merkle verification of a remote repository.
 *
 * svnae_client_verify(base_url, repo, rev) walks the tree at `rev`
 * top-down, re-hashes every file content and dir blob locally using
 * the server's advertised algorithm, and confirms:
 *
 *   1. Each file/dir node's locally-computed content hash matches
 *      the X-Svnae-Node-Hash header the server returned.
 *   2. For each directory, the re-assembled blob (lines
 *      "<kind> <child-sha> <name>\n", sorted) hashes to the same
 *      sha that the parent dir pointed at.
 *   3. The root-dir sha equals the rev blob's "root" field
 *      (served via /info).
 *
 * The interesting move is step 2: the server gives us a JSON listing
 * of a directory's entries (name + kind), but to re-hash a dir we
 * need to reconstruct its *blob*, which has the wire format
 *   <kind-char> <child-sha> <name>\n
 * We obtain each child's sha by fetching that child (cat for files,
 * list for dirs) and reading the X-Svnae-Node-Hash header. Recurse.
 *
 * The walk is depth-first from the root. We bail on the first
 * mismatch and return a negative code; callers print a diagnostic.
 *
 * Returns:
 *    0 on full verification success
 *   -1 on I/O / protocol error
 *   -2 on hash mismatch (message printed to stderr with path + details)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- forward decls from the RA shim -------------------------------- */

struct svnae_ra_info;
struct svnae_ra_info *svnae_ra_info_rev(const char *base_url, const char *repo,
                                        int rev);
int         svnae_ra_info_rev_num(const struct svnae_ra_info *I);
const char *svnae_ra_info_root   (struct svnae_ra_info *I);
void        svnae_ra_info_free   (struct svnae_ra_info *I);

char *svnae_ra_hash_algo(const char *base_url, const char *repo);

/* We need to fetch a node's raw bytes + its Merkle header.  The existing
 * RA accessors (svnae_ra_cat, svnae_ra_list) don't surface the header.
 * Rather than add a third variant to each, the verify pass speaks HTTP
 * directly via the RA shim's already-exposed http_get_ex (declared
 * here; defined in ra/shim.c). */
extern int http_get_ex(const char *url, char **out_body, size_t *out_len,
                       int *out_status, char **out_node_hash);

/* Hashing consolidated in ae/ffi/openssl/shim.c. */
extern char *svnae_openssl_hash_hex(const char *algo, const char *data, int len);
#define hash_hex svnae_openssl_hash_hex

/* Sort helper for dir entries so the re-assembled blob matches the
 * server's canonical line order. */
struct entry { char kind_c; char *name; char *sha; };

static int
entry_cmp(const void *a, const void *b)
{
    const struct entry *ea = a, *eb = b;
    return strcmp(ea->name, eb->name);
}

/* Fetch and verify a single file at `rel`. Returns 0 on match, -1 on
 * I/O error, -2 on hash mismatch. Out: `*out_sha` gets a malloc'd
 * copy of the computed sha (for the parent to thread into its own
 * blob). */
static int
verify_file(const char *base_url, const char *repo, int rev,
            const char *algo, const char *rel, char **out_sha)
{
    const char *url = aether_url_rev_cat(base_url, repo, rev, rel);
    char *body = NULL; size_t len = 0; int status = 0; char *hdr = NULL;
    if (http_get_ex(url, &body, &len, &status, &hdr) != 0 || status != 200) {
        free(body); free(hdr);
        fprintf(stderr, "verify: GET failed for %s (status %d)\n", rel, status);
        return -1;
    }
    /* Compute the file's content hash locally. Note: the server
     * currently sends NUL-terminated text and we strlen it; same
     * convention as `svnae_ra_cat`. When binary flows we'll switch
     * to len-aware. */
    int clen = (int)strlen(body);
    char *local = hash_hex(algo, body, clen);
    free(body);
    if (!local) { free(hdr); return -1; }

    if (!hdr || strcmp(local, hdr) != 0) {
        fprintf(stderr, "verify: hash mismatch at /%s\n", rel);
        fprintf(stderr, "  server: %s\n", hdr ? hdr : "(missing)");
        fprintf(stderr, "  local:  %s\n", local);
        free(hdr); free(local);
        return -2;
    }
    free(hdr);
    *out_sha = local;
    return 0;
}

/* Recursive dir verify. `rel` is the repo-relative path of the
 * directory being verified ("" for the root). The caller already
 * knows the sha the parent attributed to this dir; we compare it
 * against the dir's recomputed sha after recursion. */
static int
verify_dir(const char *base_url, const char *repo, int rev,
           const char *algo, const char *rel, char **out_sha)
{
    /* GET /rev/N/list/<path> — listing tells us the immediate children
     * (names + kinds). The response header carries the server's view
     * of *this* dir's blob sha. */
    const char *url = aether_url_rev_list(base_url, repo, rev, rel);

    char *body = NULL; size_t len = 0; int status = 0; char *dir_hdr = NULL;
    if (http_get_ex(url, &body, &len, &status, &dir_hdr) != 0 || status != 200) {
        free(body); free(dir_hdr);
        fprintf(stderr, "verify: GET /list failed for /%s (status %d)\n",
                rel, status);
        return -1;
    }

    /* Use the same packed-string accessors from ae/ra/packed.ae that
     * the public svnae_ra_list handle does — ra_list_kind returns
     * "dir"/"file" so we compare against that instead of a kind char. */
    extern const char *aether_ra_parse_list(const char *body);
    extern int         aether_ra_list_count(const char *packed);
    extern const char *aether_ra_list_name(const char *packed, int i);
    extern const char *aether_ra_list_kind(const char *packed, int i);

    const char *packed = aether_ra_parse_list(body);
    free(body);
    if (!packed || !*packed) { free(dir_hdr); return -1; }
    /* Stable copy — the Aether return lifetime ends at the next
     * string-producing call; the recursion below hits many. */
    char *packed_owned = strdup(packed);
    if (!packed_owned) { free(dir_hdr); return -1; }

    int n = aether_ra_list_count(packed_owned);
    struct entry *entries = calloc((size_t)(n > 0 ? n : 1), sizeof *entries);
    for (int i = 0; i < n; i++) {
        const char *nm = aether_ra_list_name(packed_owned, i);
        const char *kd = aether_ra_list_kind(packed_owned, i);
        entries[i].name   = strdup(nm ? nm : "");
        entries[i].kind_c = (kd && strcmp(kd, "dir") == 0) ? 'd' : 'f';
        entries[i].sha    = NULL;
    }
    free(packed_owned);

    /* Recurse into every child to compute its sha. */
    extern const char *aether_path_join_rel(const char *prefix, const char *name);
    for (int i = 0; i < n; i++) {
        const char *child_rel = aether_path_join_rel(rel, entries[i].name);

        int rc;
        if (entries[i].kind_c == 'd') {
            rc = verify_dir (base_url, repo, rev, algo, child_rel, &entries[i].sha);
        } else {
            rc = verify_file(base_url, repo, rev, algo, child_rel, &entries[i].sha);
        }
        if (rc != 0) {
            for (int j = 0; j <= i; j++) { free(entries[j].name); free(entries[j].sha); }
            for (int j = i + 1; j < n; j++) { free(entries[j].name); }
            free(entries);
            free(dir_hdr);
            return rc;
        }
    }

    /* Rebuild the dir blob (sorted by name) and hash it. Line format:
     *   <kind> <sha> <name>\n
     * matching fs_fs/txn_shim.c:rebuild_dir_c's serialiser. */
    qsort(entries, (size_t)n, sizeof *entries, entry_cmp);
    size_t buf_cap = 1024, buf_len = 0;
    char *buf = malloc(buf_cap);
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t need = 2 + strlen(entries[i].sha) + 1 + strlen(entries[i].name) + 2;
        if (buf_len + need + 1 >= buf_cap) {
            buf_cap = (buf_len + need + 1) * 2;
            buf = realloc(buf, buf_cap);
        }
        buf_len += (size_t)snprintf(buf + buf_len, buf_cap - buf_len,
                                    "%c %s %s\n",
                                    entries[i].kind_c, entries[i].sha, entries[i].name);
    }

    char *local = hash_hex(algo, buf, (int)buf_len);
    free(buf);

    /* Compare the re-hashed blob against the dir header the server
     * advertised for *this* dir. */
    int ok = (local && dir_hdr && strcmp(local, dir_hdr) == 0);
    if (!ok) {
        fprintf(stderr, "verify: dir hash mismatch at /%s\n", *rel ? rel : "");
        fprintf(stderr, "  server: %s\n", dir_hdr ? dir_hdr : "(missing)");
        fprintf(stderr, "  local:  %s\n", local ? local : "(null)");
    }
    free(dir_hdr);

    for (int i = 0; i < n; i++) { free(entries[i].name); free(entries[i].sha); }
    free(entries);

    if (!ok) { free(local); return -2; }
    *out_sha = local;
    return 0;
}

/* Crawl the tree rooted at `rel` and, for every file, fetch its
 * /hashes endpoint. For each secondary algo the server stores,
 * re-hash the content locally and compare. Returns 0 match, -1 I/O,
 * -2 mismatch. Emits a per-file OK summary to stdout with the
 * secondary-hash count when the caller asked for verbose output. */
static int
verify_secondaries_in_dir(const char *base_url, const char *repo, int rev,
                          const char *rel, int *out_files_checked,
                          int *out_secondary_count)
{
    const char *url = aether_url_rev_list(base_url, repo, rev, rel);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get_ex(url, &body, &len, &status, NULL) != 0 || status != 200) {
        free(body);
        return -1;
    }
    /* Parse entries via the already-ported ra_parse_list — same
     * "N\x02<name>\x01<kind-char>\x02..." shape verify_dir uses. */
    extern const char *aether_ra_parse_list(const char *body);
    const char *packed = aether_ra_parse_list(body);
    free(body);
    if (!packed || !*packed) return -1;

    const char *p = packed;
    const char *sep = strchr(p, 2);
    if (!sep) return -1;
    char nbuf[32];
    size_t nlen = (size_t)(sep - p);
    if (nlen >= sizeof nbuf) nlen = sizeof nbuf - 1;
    memcpy(nbuf, p, nlen); nbuf[nlen] = '\0';
    int n = (int)strtol(nbuf, NULL, 10);
    p = sep + 1;

    struct entry { char kind_c; char *name; };
    struct entry *ents = calloc((size_t)(n > 0 ? n : 1), sizeof *ents);
    for (int i = 0; i < n; i++) {
        const char *entry_end = strchr(p, 2);
        if (!entry_end) entry_end = p + strlen(p);
        const char *mid = memchr(p, 1, (size_t)(entry_end - p));
        ents[i].name   = mid ? strndup(p, (size_t)(mid - p)) : strdup("");
        ents[i].kind_c = (mid && mid + 1 < entry_end) ? *(mid + 1) : 'f';
        p = *entry_end == 2 ? entry_end + 1 : entry_end;
    }

    int overall = 0;
    extern const char *aether_path_join_rel(const char *prefix, const char *name);
    extern const char *aether_ra_parse_hashes_secondaries(const char *body);
    for (int i = 0; i < n; i++) {
        const char *child_rel = aether_path_join_rel(rel, ents[i].name);

        if (ents[i].kind_c == 'd') {
            int rc = verify_secondaries_in_dir(base_url, repo, rev, child_rel,
                                               out_files_checked, out_secondary_count);
            if (rc != 0 && overall == 0) overall = rc;
        } else {
            /* Fetch /cat and /hashes; re-hash for each declared secondary. */
            const char *cat_url = aether_url_rev_cat(base_url, repo, rev, child_rel);
            char *cbody = NULL; size_t clen = 0; int cstatus = 0;
            if (http_get_ex(cat_url, &cbody, &clen, &cstatus, NULL) != 0
                || cstatus != 200) {
                if (overall == 0) overall = -1;
                free(cbody); continue;
            }
            int body_len = (int)strlen(cbody);

            const char *h_url = aether_url_rev_hashes(base_url, repo, rev, child_rel);
            char *hbody = NULL; size_t hlen = 0; int hstatus = 0;
            if (http_get_ex(h_url, &hbody, &hlen, &hstatus, NULL) != 0
                || hstatus != 200) {
                free(cbody); free(hbody);
                if (overall == 0) overall = -1;
                continue;
            }

            /* Parse the secondaries list ported to Aether —
             * "N\x02<algo>\x01<hash>\x02..." */
            const char *sec_packed = aether_ra_parse_hashes_secondaries(hbody);
            if (sec_packed && *sec_packed) {
                const char *sp = sec_packed;
                const char *ssep = strchr(sp, 2);
                if (ssep) {
                    char snbuf[16];
                    size_t snlen = (size_t)(ssep - sp);
                    if (snlen >= sizeof snbuf) snlen = sizeof snbuf - 1;
                    memcpy(snbuf, sp, snlen); snbuf[snlen] = '\0';
                    int sec_n = (int)strtol(snbuf, NULL, 10);
                    sp = ssep + 1;
                    for (int si = 0; si < sec_n; si++) {
                        const char *entry_end = strchr(sp, 2);
                        if (!entry_end) entry_end = sp + strlen(sp);
                        const char *mid = memchr(sp, 1, (size_t)(entry_end - sp));
                        if (mid) {
                            char algo_buf[32];
                            size_t al = (size_t)(mid - sp);
                            if (al >= sizeof algo_buf) al = sizeof algo_buf - 1;
                            memcpy(algo_buf, sp, al); algo_buf[al] = '\0';
                            size_t hl = (size_t)(entry_end - (mid + 1));
                            char *server_hex = strndup(mid + 1, hl);
                            char *local = hash_hex(algo_buf, cbody, body_len);
                            if (local && strcmp(local, server_hex) == 0) {
                                (*out_secondary_count)++;
                            } else {
                                fprintf(stderr,
                                        "verify: secondary %s mismatch at /%s\n"
                                        "  server: %s\n  local:  %s\n",
                                        algo_buf, child_rel,
                                        server_hex, local ? local : "(null)");
                                if (overall == 0) overall = -2;
                            }
                            free(local); free(server_hex);
                        }
                        sp = *entry_end == 2 ? entry_end + 1 : entry_end;
                    }
                }
            }
            free(cbody); free(hbody);
            (*out_files_checked)++;
        }
    }
    for (int i = 0; i < n; i++) free(ents[i].name);
    free(ents);
    return overall;
}

/* Public entry point. Prints a short summary on success. Returns
 * 0 on match, -1 on protocol/IO error, -2 on Merkle mismatch.
 *
 * When `with_secondaries` is 1, after the primary Merkle walk passes
 * the tree is re-walked to verify every file's stored secondary
 * hashes. Mismatch in a secondary fails the whole verify. */
int
svnae_client_verify_full(const char *base_url, const char *repo, int rev,
                        int with_secondaries)
{
    char *algo = svnae_ra_hash_algo(base_url, repo);
    if (!algo || !*algo) { free(algo); fprintf(stderr, "verify: server info unavailable\n"); return -1; }

    struct svnae_ra_info *I = svnae_ra_info_rev(base_url, repo, rev);
    if (!I) { free(algo); fprintf(stderr, "verify: rev %d info unavailable\n", rev); return -1; }
    const char *claimed_root = svnae_ra_info_root(I);
    if (!*claimed_root) {
        svnae_ra_info_free(I); free(algo);
        fprintf(stderr, "verify: rev %d has no root sha in /info\n", rev);
        return -1;
    }
    char *claimed_root_copy = strdup(claimed_root);
    svnae_ra_info_free(I);

    char *root_sha = NULL;
    int rc = verify_dir(base_url, repo, rev, algo, "", &root_sha);
    if (rc != 0) {
        free(algo); free(root_sha); free(claimed_root_copy);
        return rc;
    }

    if (!root_sha || strcmp(root_sha, claimed_root_copy) != 0) {
        fprintf(stderr, "verify: root sha mismatch\n");
        fprintf(stderr, "  rev-blob root: %s\n", claimed_root_copy);
        fprintf(stderr, "  recomputed:    %s\n", root_sha ? root_sha : "(null)");
        free(algo); free(root_sha); free(claimed_root_copy);
        return -2;
    }

    int sec_rc = 0;
    int files = 0, sec_count = 0;
    if (with_secondaries) {
        sec_rc = verify_secondaries_in_dir(base_url, repo, rev, "", &files, &sec_count);
    }

    if (sec_rc != 0) {
        fprintf(stderr, "verify: secondary check failed (rc=%d)\n", sec_rc);
        free(algo); free(root_sha); free(claimed_root_copy);
        return sec_rc;
    }

    if (with_secondaries) {
        fprintf(stdout, "verify: OK (algo=%s, root=%s, %d file(s), %d secondary hash(es) verified)\n",
                algo, root_sha, files, sec_count);
    } else {
        fprintf(stdout, "verify: OK (algo=%s, root=%s)\n", algo, root_sha);
    }
    free(algo); free(root_sha); free(claimed_root_copy);
    return 0;
}

int
svnae_client_verify(const char *base_url, const char *repo, int rev)
{
    return svnae_client_verify_full(base_url, repo, rev, 0);
}
