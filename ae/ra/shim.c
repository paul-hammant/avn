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
 * Mirrors repos/shim.c's query shape but over HTTP + JSON. Handles
 * returned here (svnae_ra_log, svnae_ra_list, svnae_ra_info) match
 * the server-side equivalents so callers can switch repository
 * sources without reshape.
 *
 * HTTP plumbing lives in ae/ra/http_client.ae (std.http.client v2);
 * URL builders in urls.ae; JSON parse/build in parse.ae and
 * commit_build.ae. The C side keeps the public ABI and the auth-
 * state TLS pointers (X-Svnae-User / X-Svnae-Superuser tokens). */

#include "aether_json.h"    /* JsonValue / json_create_number used by the
                               svnae_ra_json_int helper the commit-build
                               Aether module calls back into. */
#include "aether_string.h"  /* aether_string_data / aether_string_length */
#include "../subr/pin_list.h"
#include "../subr/packed_handle/packed_handle.h"
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
extern const char *aether_url_info(const char *base, const char *repo);
extern const char *aether_url_log(const char *base, const char *repo);
extern const char *aether_url_commit(const char *base, const char *repo);
extern const char *aether_url_rev_info(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_paths(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_cat(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_list(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_props(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_blame(const char *base, const char *repo, int rev, const char *path);

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

/* The libcurl-using helpers moved to ae/ra/http_client.ae. The Aether
 * helper returns a std.http.client response handle (ptr) we read via
 * typed accessors — status, body, header — and then free. */
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

/* GET `url`. Returns 0 on success (body malloc'd, caller frees),
 * -1 on transport failure. */
static int
http_get(const char *url, char **out_body, size_t *out_len, int *out_status)
{
    const void *resp = aether_ra_http_get(url);
    if (!resp) return -1;
    *out_status = aether_ra_http_response_status(resp);
    int rc = take_body(aether_ra_http_response_body(resp), out_body, out_len);
    aether_ra_http_response_free(resp);
    return rc;
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
    int rev = aether_ra_parse_head_rev(body);
    free(body);
    return rev;
}

/* Query the server's primary content-address algorithm. Returns a
 * malloc'd string (caller frees) or NULL on failure. Pre-Phase-6.1
 * servers omit the field; the Aether parser defaults to "sha1". */
extern const char *aether_ra_parse_hash_algo(const char *body);
char *
svnae_ra_hash_algo(const char *base_url, const char *repo_name)
{
    char *body = http_get_200(aether_url_info(base_url, repo_name), NULL);
    if (!body) return NULL;
    const char *algo = aether_ra_parse_hash_algo(body);
    free(body);
    if (!algo || !*algo) return NULL;
    return strdup(algo);
}

/* ---- packed-record handle internals ---------------------------------
 *
 * ae/ra/parse.ae produces packed "<N>\x02<entry>\x02..." payloads;
 * ae/ra/packed.ae exposes typed accessors over each entry's
 * "<f0>\x01<f1>\x01..." record. Per-domain handles (log/paths/
 * blame/list) all share the same struct {packed, n, pins} shape
 * — defined in ae/subr/packed_handle, shared with repos/shim.c.
 * ra_handle_from_url is the URL-fetch + parse + handle-construct
 * layer over svnae_packed_handle_new; per-domain wrappers below
 * cast to/from the public per-domain types. */

typedef const char *(*ra_parse_fn)(const char *body);
typedef int         (*ra_count_fn)(const char *packed);

static struct svnae_packed_handle *
ra_handle_from_url(const char *url, ra_parse_fn parse, ra_count_fn count)
{
    char *body = http_get_200(url, NULL);
    if (!body) return NULL;
    const char *packed = parse(body);
    struct svnae_packed_handle *h = svnae_packed_handle_new(packed, count);
    free(body);
    return h;
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

int svnae_ra_log_count(const struct svnae_ra_log *lg) { return svnae_packed_count(lg); }
int svnae_ra_log_rev   (const struct svnae_ra_log *lg, int i) { return svnae_packed_int_at(lg, i, aether_ra_log_rev); }
const char *svnae_ra_log_author(struct svnae_ra_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_author); }
const char *svnae_ra_log_date  (struct svnae_ra_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_date); }
const char *svnae_ra_log_msg   (struct svnae_ra_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_msg); }
void svnae_ra_log_free(struct svnae_ra_log *lg) { svnae_packed_handle_free((struct svnae_packed_handle *)lg); }

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

int svnae_ra_paths_count (const struct svnae_ra_paths *P) { return svnae_packed_count(P); }
const char *svnae_ra_paths_action(struct svnae_ra_paths *P, int i) { return svnae_packed_pin_at(P, i, aether_ra_paths_action); }
const char *svnae_ra_paths_path  (struct svnae_ra_paths *P, int i) { return svnae_packed_pin_at(P, i, aether_ra_paths_path); }
void svnae_ra_paths_free(struct svnae_ra_paths *P) { svnae_packed_handle_free((struct svnae_packed_handle *)P); }

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

int svnae_ra_blame_count (const struct svnae_ra_blame *B) { return svnae_packed_count(B); }
int svnae_ra_blame_rev   (const struct svnae_ra_blame *B, int i) { return svnae_packed_int_at(B, i, aether_ra_blame_rev); }
const char *svnae_ra_blame_author(struct svnae_ra_blame *B, int i) { return svnae_packed_pin_at(B, i, aether_ra_blame_author); }
const char *svnae_ra_blame_text  (struct svnae_ra_blame *B, int i) { return svnae_packed_pin_at(B, i, aether_ra_blame_text); }
void svnae_ra_blame_free(struct svnae_ra_blame *B) { svnae_packed_handle_free((struct svnae_packed_handle *)B); }

/* ---- info handle ----------------------------------------------------- */

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
    struct svnae_packed_handle *h = svnae_packed_handle_new(packed, NULL);
    free(body);
    return (struct svnae_ra_info *)h;
}

int svnae_ra_info_rev_num (const struct svnae_ra_info *I) { return svnae_packed_int_field(I, aether_ra_info_rev); }
const char *svnae_ra_info_author(struct svnae_ra_info *I) { return svnae_packed_pin_field(I, aether_ra_info_author); }
const char *svnae_ra_info_date  (struct svnae_ra_info *I) { return svnae_packed_pin_field(I, aether_ra_info_date); }
const char *svnae_ra_info_msg   (struct svnae_ra_info *I) { return svnae_packed_pin_field(I, aether_ra_info_msg); }
const char *svnae_ra_info_root  (struct svnae_ra_info *I) { return svnae_packed_pin_field(I, aether_ra_info_root); }
void        svnae_ra_info_free  (struct svnae_ra_info *I) { svnae_packed_handle_free((struct svnae_packed_handle *)I); }

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

extern const char *aether_ra_parse_props(const char *body);
extern int         aether_ra_props_count(const char *packed);
extern const char *aether_ra_props_name(const char *packed, int i);
extern const char *aether_ra_props_value(const char *packed, int i);

struct svnae_ra_props *
svnae_ra_get_props(const char *base_url, const char *repo_name,
                   int rev, const char *path)
{
    while (*path == '/') path++;
    return (struct svnae_ra_props *)ra_handle_from_url(
        aether_url_rev_props(base_url, repo_name, rev, path),
        aether_ra_parse_props, aether_ra_props_count);
}

int svnae_ra_props_count(const struct svnae_ra_props *P) { return svnae_packed_count(P); }
const char *svnae_ra_props_name (struct svnae_ra_props *P, int i) { return svnae_packed_pin_at(P, i, aether_ra_props_name); }
const char *svnae_ra_props_value(struct svnae_ra_props *P, int i) { return svnae_packed_pin_at(P, i, aether_ra_props_value); }
void        svnae_ra_props_free (struct svnae_ra_props *P) { svnae_packed_handle_free((struct svnae_packed_handle *)P); }

/* svnae_ra_server_copy and svnae_ra_branch_create moved to
 * ae/ra/copy_branch.ae in Round 78. Both go through the adapter
 * below, which handles the std.http.client response object on this
 * side and returns a "<status>\x01<body>" packed string the Aether
 * caller splits. */
const char *
svnae_ra_http_post_status_body(const char *url, const char *body)
{
    static __thread char *last = NULL;
    free(last); last = NULL;

    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, body, &resp, &len, &status);
    (void)len;

    if (rc != 0) {
        last = strdup("0\x01");
        return last ? last : "";
    }
    const char *body_str = resp ? resp : "";
    int n = (int)snprintf(NULL, 0, "%d", status) + 1 + (int)strlen(body_str) + 1;
    last = malloc((size_t)n);
    if (!last) { free(resp); return ""; }
    snprintf(last, (size_t)n, "%d\x01%s", status, body_str);
    free(resp);
    return last;
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

int svnae_ra_list_count(const struct svnae_ra_list *L) { return svnae_packed_count(L); }
const char *svnae_ra_list_name(struct svnae_ra_list *L, int i) { return svnae_packed_pin_at(L, i, aether_ra_list_name); }
const char *svnae_ra_list_kind(struct svnae_ra_list *L, int i) { return svnae_packed_pin_at(L, i, aether_ra_list_kind); }
void svnae_ra_list_free(struct svnae_ra_list *L) { svnae_packed_handle_free((struct svnae_packed_handle *)L); }

/* ---- commit ---------------------------------------------------------- *
 *
 * Builder pattern: begin → add_file/mkdir/delete/set_prop/acl_add →
 * finish (which serialises + POSTs). Three packed-string buffers
 * (edits/props/acls) parallel ra/parse.ae's "<count>\x02<rec>\x02..."
 * shape so commit_build.ae walks each once via string.split. b64-
 * encoding happens at add-file time so the buffer carries text only. */

struct svnae_ra_commit {
    int   base_rev;
    char *author;
    char *logmsg;
    char *edits_packed;   /* "<n>\x02<op>\x01<path>\x01<b64>\x02..."   */
    int   n_edits;
    char *props_packed;   /* "<n>\x02<path>\x01<key>\x01<value>\x02..." */
    int   n_props;
    char *acls_packed;    /* "<n>\x02<path>\x01<rule>\x02..."          */
    int   n_acls;
};

/* Append `record` (a "\x01"-joined field string) as one entry in `*buf`.
 * Updates `*count`. Buffer grows to "<count>\x02<rec0>\x02<rec1>\x02..."
 * Initial empty buffer becomes "1\x02<rec0>\x02"; subsequent appends
 * rewrite the leading count and tack the new record + trailing \x02. */
static int
pack_append(char **buf, int *count, const char *record)
{
    if (!record) record = "";
    int new_count = *count + 1;
    char header[16];
    int hlen = snprintf(header, sizeof header, "%d", new_count);
    int rlen = (int)strlen(record);

    /* New size: header + \x02 + every existing record (between the
     * leading "<old_count>\x02" and trailing \x02) + record + \x02 + NUL. */
    const char *existing_body = "";
    int body_len = 0;
    if (*buf) {
        const char *first_sep = strchr(*buf, '\x02');
        if (first_sep) {
            existing_body = first_sep + 1;
            body_len = (int)strlen(existing_body);
        }
    }

    int total = hlen + 1 + body_len + rlen + 1 + 1;
    char *out = malloc((size_t)total);
    if (!out) return -1;
    memcpy(out, header, (size_t)hlen);
    out[hlen] = '\x02';
    memcpy(out + hlen + 1, existing_body, (size_t)body_len);
    memcpy(out + hlen + 1 + body_len, record, (size_t)rlen);
    out[hlen + 1 + body_len + rlen] = '\x02';
    out[hlen + 1 + body_len + rlen + 1] = '\0';

    free(*buf);
    *buf = out;
    *count = new_count;
    return 0;
}

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

/* Consolidated in ae/ffi/openssl/shim.c. */
extern char *svnae_openssl_b64_encode(const unsigned char *src, int len);

int
svnae_ra_commit_add_file(struct svnae_ra_commit *cb, const char *path,
                         const char *content, int len)
{
    if (!cb) return -1;
    char *b64 = (len > 0 && content)
        ? svnae_openssl_b64_encode((const unsigned char *)content, len)
        : strdup("");
    if (!b64) return -1;
    int rlen = (int)strlen(path) + 1 + 1 + 1 + (int)strlen(b64) + 1;
    char *rec = malloc((size_t)rlen);
    if (!rec) { free(b64); return -1; }
    snprintf(rec, (size_t)rlen, "1\x01%s\x01%s", path, b64);
    int rc = pack_append(&cb->edits_packed, &cb->n_edits, rec);
    free(rec); free(b64);
    return rc;
}

int
svnae_ra_commit_mkdir(struct svnae_ra_commit *cb, const char *path)
{
    if (!cb) return -1;
    int rlen = (int)strlen(path) + 1 + 1 + 1 + 1;
    char *rec = malloc((size_t)rlen);
    if (!rec) return -1;
    snprintf(rec, (size_t)rlen, "2\x01%s\x01", path);
    int rc = pack_append(&cb->edits_packed, &cb->n_edits, rec);
    free(rec);
    return rc;
}

int
svnae_ra_commit_delete(struct svnae_ra_commit *cb, const char *path)
{
    if (!cb) return -1;
    int rlen = (int)strlen(path) + 1 + 1 + 1 + 1;
    char *rec = malloc((size_t)rlen);
    if (!rec) return -1;
    snprintf(rec, (size_t)rlen, "3\x01%s\x01", path);
    int rc = pack_append(&cb->edits_packed, &cb->n_edits, rec);
    free(rec);
    return rc;
}

int
svnae_ra_commit_acl_add(struct svnae_ra_commit *cb,
                        const char *path, const char *rule)
{
    if (!cb) return -1;
    int rlen = (int)strlen(path) + 1 + (int)strlen(rule) + 1;
    char *rec = malloc((size_t)rlen);
    if (!rec) return -1;
    snprintf(rec, (size_t)rlen, "%s\x01%s", path, rule);
    int rc = pack_append(&cb->acls_packed, &cb->n_acls, rec);
    free(rec);
    return rc;
}

int
svnae_ra_commit_set_prop(struct svnae_ra_commit *cb,
                         const char *path, const char *key, const char *value)
{
    if (!cb) return -1;
    int rlen = (int)strlen(path) + 1 + (int)strlen(key) + 1
             + (int)strlen(value) + 1;
    char *rec = malloc((size_t)rlen);
    if (!rec) return -1;
    snprintf(rec, (size_t)rlen, "%s\x01%s\x01%s", path, key, value);
    int rc = pack_append(&cb->props_packed, &cb->n_props, rec);
    free(rec);
    return rc;
}

/* --- Aether-callable accessors -------------------------------------- *
 *
 * 14 typed getters collapsed to 6 packed-string + 3 count fields.
 * commit_build.ae walks each packed buffer with std.string.split. */
int         svnae_ra_cb_base_rev(const struct svnae_ra_commit *cb) {
    return cb ? cb->base_rev : 0;
}
const char *svnae_ra_cb_author(const struct svnae_ra_commit *cb) {
    return (cb && cb->author) ? cb->author : "";
}
const char *svnae_ra_cb_logmsg(const struct svnae_ra_commit *cb) {
    return (cb && cb->logmsg) ? cb->logmsg : "";
}
const char *svnae_ra_cb_edits_packed(const struct svnae_ra_commit *cb) {
    return (cb && cb->edits_packed) ? cb->edits_packed : "";
}
const char *svnae_ra_cb_props_packed(const struct svnae_ra_commit *cb) {
    return (cb && cb->props_packed) ? cb->props_packed : "";
}
const char *svnae_ra_cb_acls_packed(const struct svnae_ra_commit *cb) {
    return (cb && cb->acls_packed) ? cb->acls_packed : "";
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

    free(cb->author);
    free(cb->logmsg);
    free(cb->edits_packed);
    free(cb->props_packed);
    free(cb->acls_packed);
    free(cb);

    return new_rev;
}

/* ---- verify glue ----------------------------------------------------
 *
 * The Merkle-verify walk lives in ae/ra/verify.ae. The two C-side
 * struct allocators below back it:
 *   svnae_verify_counter:  (files, secondaries) accumulator
 *   svnae_verify_entries:  (name, kind_c, sha) tuples sortable by name
 * Storage in C since Aether can't allocate struct-of-arrays with
 * ergonomic field access from FFI. */

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
