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

/* svnserver/shim.c — HTTP handlers for the svn-aether read-only server.
 *
 * Routes (all GET):
 *   /repos/{r}/info                         -> {"head":N,"uuid":"..."}
 *   /repos/{r}/log                          -> {"entries":[{rev,author,date,msg},...]}
 *   /repos/{r}/rev/:rev/info                -> {"rev":N,"author":"...",...}
 *   /repos/{r}/rev/:rev/list/*path          -> {"entries":[{"name":"...","kind":"..."},...]}
 *   /repos/{r}/rev/:rev/cat/*path           -> raw bytes
 *
 * Single-repo mode for Phase 6: we map the one repo name in the URL to a
 * single on-disk path configured at startup. Multi-repo via a table comes
 * when we have config parsing.
 *
 * The JSON we produce is hand-rolled: we only need to serialise strings,
 * ints, and arrays of objects. Using std.json from C is awkward because
 * the stdlib's JSON builder allocates Aether-managed objects. We escape
 * strings ourselves (minimal: \" \\ \n \r \t \b \f and any other control
 * as \uXXXX).
 */

/* JSON parse + build live entirely in Aether now
 * (ae/svnserver/branch_create_parse.ae, copy_parse.ae,
 * commit_parse.ae). No std.json is consumed by this file directly. */

#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- forward decls from other shims ------------------------------------ */

struct svnae_txn;

/* Only functions this C file actually calls — the Aether modules
 * that live alongside it declare what they need locally. */
int  svnae_txn_add_file(struct svnae_txn *t, const char *path, const char *content, int len);
int  svnae_branch_spec_allows(const char *repo, const char *branch, const char *path);

extern const char *aether_error_response_json(const char *msg);

/* Functions this file actually calls (everything else reaches the
 * C symbol at link time from another translation unit, no forward
 * decl required). */
const char *svnae_repo_primary_hash(const char *repo);
int         svnae_repo_secondary_hashes(const char *repo, char out[4][32]);
char       *svnae_rep_lookup_secondary(const char *repo,
                                       const char *primary_hex,
                                       const char *algo);

/* Aether HTTP server types we reach into. Must match the layout in
 * aether/std/net/aether_http_server.h exactly. */
typedef struct {
    char* method;
    char* path;
    char* query_string;
    char* http_version;
    char** header_keys;
    char** header_values;
    int header_count;
    char* body;
    size_t body_length;
    char** param_keys;
    char** param_values;
    int param_count;
} HttpRequest;

typedef struct {
    int status_code;
    char* status_text;
    char** header_keys;
    char** header_values;
    int header_count;
    char* body;
    size_t body_length;
} HttpServerResponse;

void http_response_set_status(HttpServerResponse *res, int code);
void http_response_set_header(HttpServerResponse *res, const char *k, const char *v);
void http_response_set_body(HttpServerResponse *res, const char *body);
void http_response_json(HttpServerResponse *res, const char *json);

/* --- authorization (Phase 7.1) --------------------------------------
 *
 * Rules live in the rev blob as an `acl: <paths-acl-sha>` field. The
 * paths-acl blob lists "<acl-sha> <path>" lines; each per-path ACL
 * blob has "+user" or "-user" lines (also "+*" / "-*" wildcard).
 *
 * Inheritance: a path's effective ACL is the nearest ancestor (or
 * self) with an entry. If no ancestor has one, the tree is open.
 *
 * Placeholder auth: X-Svnae-User: <name> — trusted verbatim. Missing
 * header = anonymous. Super-user bypass: X-Svnae-Superuser: <token>
 * matches the server's --superuser-token flag. */

static char *g_superuser_token = NULL;

void
svnae_svnserver_set_superuser_token(const char *token)
{
    free(g_superuser_token);
    g_superuser_token = token && *token ? strdup(token) : NULL;
}

/* Case-insensitive header lookup via std.http. */
extern const char *http_get_header(HttpRequest *req, const char *name);
#define req_header http_get_header

/* ACL rule evaluation + ancestry-walking mode check live in
 * ae/svnserver/acl_mode.ae and acl_resolve.ae. Rule syntax:
 *   +alice / +alice:r / +alice:w / +alice:rw / +*
 *   -alice (deny is absolute regardless of mode) / -*
 * 1 allow, 0 deny, -1 no-match. Explicit user beats wildcard;
 * deny beats allow at the same precedence. */
extern int aether_acl_allows_mode(const char *repo, int rev,
                                  const char *user, const char *target_path,
                                  int want_write);

extern int svnae_openssl_hash_hex_into(const char *algo, const char *data, int len, char *out);

const char *svnserver_hash_hex(const char *repo, const char *data, int length) {
    static __thread char buf[65];
    buf[0] = '\0';
    svnae_openssl_hash_hex_into(svnae_repo_primary_hash(repo),
                                data ? data : "", length, buf);
    return buf;
}

/* Effective username for this request (X-Svnae-User header verbatim;
 * empty string for anonymous). Cached in a TLS buffer so the returned
 * pointer is stable until the next call on this thread. */
const char *svnserver_request_user(HttpRequest *req) {
    static __thread char user_cache[128];
    const char *hdr = req_header(req, "X-Svnae-User");
    if (!hdr) { user_cache[0] = '\0'; return user_cache; }
    size_t n = strlen(hdr);
    if (n >= sizeof user_cache) n = sizeof user_cache - 1;
    memcpy(user_cache, hdr, n);
    user_cache[n] = '\0';
    return user_cache;
}

/* 1 if this request carries a valid X-Svnae-Superuser token, 0 otherwise. */
int svnserver_request_is_super(HttpRequest *req) {
    const char *hdr = req_header(req, "X-Svnae-Superuser");
    if (!hdr || !g_superuser_token) return 0;
    return strcmp(hdr, g_superuser_token) == 0 ? 1 : 0;
}

/* --- repo path registry ------------------------------------------------ *
 *
 * Phase 6 single-repo mode: one name → one path. Set once at startup via
 * svnae_svnserver_register_repo(). Lookup via find_repo_path().
 */

static char *g_repo_name = NULL;
static char *g_repo_path = NULL;

void
svnae_svnserver_register_repo(const char *name, const char *path)
{
    free(g_repo_name);
    free(g_repo_path);
    g_repo_name = strdup(name);
    g_repo_path = strdup(path);
}

/* Returns "" for missing (Aether can't return NULL from a string extern). */
const char *svnserver_find_repo_path(const char *name) {
    if (!g_repo_name || !g_repo_path) return "";
    return strcmp(name, g_repo_name) == 0 ? g_repo_path : "";
}

/* Response-emitting helpers for Aether. Each binds against std.http
 * directly; no intervening static wrappers since the dispatch + route
 * handlers all live on the Aether side now. */
void svnserver_respond_error(HttpServerResponse *res, int code, const char *msg) {
    http_response_set_status(res, code);
    http_response_json(res, aether_error_response_json(msg ? msg : ""));
}

void svnserver_respond_json_ok(HttpServerResponse *res, const char *body) {
    http_response_set_status(res, 200);
    http_response_json(res, body);
}

/* Binary body (cat). std.http's set_body strdup+strlen's the payload,
 * so we poke res->body directly to preserve byte-length for NULs. */
void svnserver_respond_binary_ok(HttpServerResponse *res, const char *data,
                                 int length, const char *content_type) {
    http_response_set_status(res, 200);
    http_response_set_header(res, "Content-Type", content_type);
    free(res->body);
    res->body = malloc((size_t)length + 1);
    if (res->body) {
        memcpy(res->body, data, (size_t)length);
        res->body[length] = '\0';
        res->body_length = (size_t)length;
    } else {
        res->body_length = 0;
    }
    char lenbuf[32];
    snprintf(lenbuf, sizeof lenbuf, "%d", length);
    http_response_set_header(res, "Content-Length", lenbuf);
}

/* Merkle verification headers for file/dir responses. */
void svnserver_set_merkle_headers(HttpServerResponse *res, const char *algo,
                                  const char *kind, const char *sha) {
    if (algo && *algo) http_response_set_header(res, "X-Svnae-Hash-Algo", algo);
    if (kind && *kind) http_response_set_header(res, "X-Svnae-Node-Kind", kind);
    if (sha  && *sha)  http_response_set_header(res, "X-Svnae-Node-Hash", sha);
}

/* Aether-callable helper: newline-separated "algo hash\n" pairs for each
 * configured secondary hash that the repo actually has for `node_sha`.
 * Returned buffer is heap-allocated (free via svnae_rep_free). "" on none. */
const char *svnserver_build_secondary_pairs(const char *repo, const char *node_sha) {
    char sec[4][32];
    int sec_n = svnae_repo_secondary_hashes(repo, sec);
    size_t cap = 512, slen = 0;
    char *pairs = malloc(cap);
    if (!pairs) return "";
    pairs[0] = '\0';
    for (int i = 0; i < sec_n; i++) {
        char *shex = svnae_rep_lookup_secondary(repo, node_sha, sec[i]);
        if (shex && *shex) {
            size_t need = strlen(sec[i]) + 1 + strlen(shex) + 2;
            if (slen + need >= cap) { cap = (slen + need) * 2; pairs = realloc(pairs, cap); }
            slen += (size_t)snprintf(pairs + slen, cap - slen, "%s %s\n", sec[i], shex);
        }
        free(shex);
    }
    return pairs;
}

/* URL parsing, dispatcher, route handlers, JSON body parsers,
 * load_rev_blob_field, and the props-handler building blocks all
 * moved to Aether — see ae/svnserver/{dispatch, handle_repo_rev,
 * handler_*, rev_load, acl_resolve, based_on_check, json}.ae. The
 * C-side trampolines that used to wrap them are gone. What remains
 * in this file: static C helpers that the Aether handlers reach via
 * extern (request header lookup, auth context, repo registry,
 * std.http response shaping, secondary-pairs builder, txn body-bytes
 * adapter). */

extern int svnae_openssl_b64_decode(const char *src, int src_len,
                                    unsigned char **out, int *out_len);
#define b64_decode svnae_openssl_b64_decode

/* Aether-callable wrappers around the static request-inspection and
 * mutation-policy helpers above. Each returns "" for a NULL string so
 * Aether's string externs can distinguish "header absent" from
 * "header present + empty" by string.length == 0. */
const char *svnserver_request_header(HttpRequest *req, const char *name) {
    const char *v = req_header(req, name);
    return (v && *v) ? v : "";
}
/* Phase 8.2b: pick the caller's branch for a mutation. Clients pass
 * Svn-Branch: <name> when they're working against a non-main branch;
 * absent header defaults to "main" (which on seeded repos with no
 * spec means "allow everything", preserving legacy behaviour). */
const char *svnserver_request_branch(HttpRequest *req) {
    const char *b = req_header(req, "Svn-Branch");
    return (b && *b) ? b : "main";
}
int svnserver_spec_allows(const char *repo, const char *branch,
                           const char *path, int is_super) {
    if (is_super) return 1;
    return svnae_branch_spec_allows(repo, branch, path) == 1;
}
/* svnserver_based_on_check trampoline retired in Round 72. Aether
 * callers (handler_path_put / handler_path_delete) now call
 * aether_based_on_check directly. */

/* Aether can't safely carry an arbitrary binary body (its `string` is
 * NUL-terminated), so the txn-add-file call that consumes req->body
 * stays on the C side. Aether calls this with a built txn; C pulls
 * body + length off the request directly. */
int svnserver_txn_add_file_from_req(void *txn, const char *path,
                                     HttpRequest *req) {
    const char *body = req->body ? req->body : "";
    int blen = req->body ? (int)req->body_length : 0;
    return svnae_txn_add_file((struct svnae_txn *)txn, path, body, blen);
}

/* Pull body bytes from req into an Aether-safe view. Returns "" if
 * the body is absent. For branch-create, the body is JSON (no embedded
 * NULs) so a NUL-terminated string round-trip is fine. */
const char *svnserver_request_body(HttpRequest *req) {
    return req->body ? req->body : "";
}
int svnserver_request_body_length(HttpRequest *req) {
    return req->body ? (int)req->body_length : 0;
}

/* Parse {base:"...", include:[...]} JSON body and perform the branch
 * create. Return value encodes the outcome for the Aether caller:
 *    >= 0 : new_rev on success
 *    -1   : malformed JSON
 *    -2   : missing/bad base/include fields
 *    -3   : include array empty
 *    -4   : svnae_branch_create rejected (exists, bad base, no globs)
 * The Aether handler maps each negative value to the appropriate HTTP
 * error response. Keeps the cJSON + glob-array bits on the C side
 * since Aether doesn't have array-of-string FFI. */
/* --- Newline-joined wrappers around the array-taking blob builders.
 * Aether can't pass const char** directly, so we pass two parallel
 * \n-separated strings (same length in line count). These are used
 * by the Aether commit-body handler to build paths-props and
 * paths-acl blobs without an FFI array type. */

/* Blob builders (props, acl, paths-props, paths-acl) ported to
 * ae/svnserver/blob_build.ae. Aether now emits the "key=value\n" /
 * "+user\n" / "<sha> <path>\n" bodies directly from its internal
 * representation and calls svnae_rep_write_blob; the round trip
 * through C-side split_joined + svnae_build_*_blob is gone. */

/* base64 decode wrapper for Aether: decodes in-place-ish, calls
 * svnae_txn_add_file with the decoded bytes+length. Returns 0 on
 * success, -1 on base64 decode failure. Keeps the byte-length-
 * aware txn_add_file call in C. */
int svnserver_txn_add_b64(void *txn, const char *path, const char *b64) {
    if (!b64) return -1;
    unsigned char *raw = NULL;
    int raw_len = 0;
    if (b64_decode(b64, (int)strlen(b64), &raw, &raw_len) != 0) return -1;
    svnae_txn_add_file((struct svnae_txn *)txn, path, (const char *)raw, raw_len);
    free(raw);
    return 0;
}

/* svnserver_branch_create_globs / _from_body, svnserver_commit_from_
 * body, svnserver_copy_from_body, svnae_svnserver_handler_dispatch
 * trampolines all removed in earlier rounds — Aether handlers now
 * dispatch directly via @c_callback annotations or are bound to
 * std.http routes by main.ae. The dispatcher itself
 * (ae/svnserver/dispatch.ae::svnserver_dispatch) is one such
 * @c_callback export. Auto-follow-copy-ACL similarly: copy_parse.ae
 * declares `extern auto_follow_copy_acl` and reaches the Aether
 * export in copy_acl.ae directly — the static C wrapper that used
 * to strdup-on-hit was redundant once the empty-string-on-miss
 * convention was adopted on the Aether side. */
