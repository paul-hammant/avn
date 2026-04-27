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

/* svnserver/shim.c — C-side helpers for the svn-aether HTTP server.
 *
 * Routes, dispatch, JSON parse/build, and per-route handlers all live
 * in ae/svnserver/{dispatch,handler_*,*_parse,*_json}.ae. What's
 * here: TLS-buffered request peeks (header / branch / user / body),
 * the superuser-token + repo-name registry, response wrappers bound
 * to std.http types, and binary-body adapters that Aether's
 * NUL-terminated `string` can't safely carry across FFI. */

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

/* ACL rule evaluation + ancestry walking live in ae/svnserver/
 * acl_mode.ae and acl_resolve.ae; .ae callers reach
 * aether_acl_allows_mode directly without going through this file. */

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
 * Single-repo mode: one name → one path. Set once at startup via
 * svnae_svnserver_register_repo, looked up via svnae_svnserver_find_repo
 * _path. Multi-repo via a config table is a future expansion. */

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
/* svnserver_respond_error / svnserver_respond_json_ok / svnserver_
 * set_merkle_headers moved to ae/svnserver/respond.ae in Round 103
 * — they were thin pass-throughs onto std.http calls and didn't
 * need the C boundary.
 *
 * svnserver_respond_binary_ok below stays C-side because std.http's
 * set_body strdup+strlen's the payload, so we poke res->body
 * directly to preserve byte-length for NULs. */
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

/* svnserver_set_merkle_headers moved to ae/svnserver/respond.ae. */

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

/* svnae_openssl_b64_decode + svnserver_txn_add_b64 retired in
 * Round 110 — the b64 decode + txn-add path now lives entirely
 * in commit_parse.ae's txn_add_b64_ helper, calling
 * svnae_crypto_b64_decode_capture / _len + svnae_txn_add_file_aether
 * directly. */

/* Header value or "" — Aether tests string.length == 0 to distinguish
 * "header absent" from "header present + empty" since string externs
 * can't return NULL. */
const char *svnserver_request_header(HttpRequest *req, const char *name) {
    const char *v = req_header(req, name);
    return (v && *v) ? v : "";
}
/* Branch this mutation targets. Clients pass Svn-Branch when working
 * against a non-main branch; absent header → "main" (open by default
 * on seeded repos with no spec). */
const char *svnserver_request_branch(HttpRequest *req) {
    const char *b = req_header(req, "Svn-Branch");
    return (b && *b) ? b : "main";
}
int svnserver_spec_allows(const char *repo, const char *branch,
                           const char *path, int is_super) {
    if (is_super) return 1;
    return svnae_branch_spec_allows(repo, branch, path) == 1;
}
/* Pull body bytes off req for an Aether-side handler. The txn-add-file
 * variant consumes the request's binary body (which Aether's
 * NUL-terminated `string` can't safely carry across FFI) and forwards
 * to svnae_txn_add_file with explicit length. */
int svnserver_txn_add_file_from_req(void *txn, const char *path,
                                     HttpRequest *req) {
    const char *body = req->body ? req->body : "";
    int blen = req->body ? (int)req->body_length : 0;
    return svnae_txn_add_file((struct svnae_txn *)txn, path, body, blen);
}

/* Body view for handlers that accept JSON (no embedded NULs). Returns
 * "" when body is absent. */
const char *svnserver_request_body(HttpRequest *req) {
    return req->body ? req->body : "";
}
int svnserver_request_body_length(HttpRequest *req) {
    return req->body ? (int)req->body_length : 0;
}

/* svnserver_txn_add_b64 retired — see top-of-file comment. */
