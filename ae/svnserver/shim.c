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
 * here: superuser-token + repo-name registry, request-header peek,
 * binary-body adapters that Aether's NUL-terminated `string` can't
 * safely carry across FFI, and the request_is_super check. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- forward decls from other shims ------------------------------------ */

struct svnae_txn;

/* Only functions this C file actually calls — the Aether modules
 * that live alongside it declare what they need locally. */
int  svnae_txn_add_file(struct svnae_txn *t, const char *path, const char *content, int len);
extern int aether_branch_spec_allows(const char *repo, const char *branch, const char *path);

/* Functions this file actually calls (everything else reaches the
 * C symbol at link time from another translation unit, no forward
 * decl required). */
extern const char *aether_repo_primary_hash(const char *repo);
/* svnae_repo_secondary_hashes / svnae_rep_lookup_secondary forward
 * decls retired alongside svnserver_build_secondary_pairs in
 * Round 136. */

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

/* svnserver_hash_hex retired in Round 134 — was a TLS-buf detour
 * over svnae_openssl_hash_hex_into. .ae callers now resolve algo
 * via aether_repo_primary_hash and call svnae_crypto_hash_hex
 * (Aether-native, returns AetherString) directly. */

/* svnserver_request_user retired in Round 141 — was a TLS-cache
 * around req_header(req, "X-Svnae-User"). Sibling
 * svnserver_request_header doesn't cache, and req's own header
 * storage outlives every handler call, so the TLS-cache shape was
 * unnecessary belt-and-braces. .ae callers now invoke
 * svnserver_request_header(req, "X-Svnae-User") directly. */

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

/* svnserver_respond_error / _respond_json_ok / _set_merkle_headers
 * moved to respond.ae in Round 103. svnserver_respond_binary_ok
 * moved to respond.ae in Round 162 once
 * http.response_set_body_n landed in Aether [current] — see
 * std_remaining_gaps.md Gap 3. */

/* svnserver_build_secondary_pairs retired in Round 136 — moved to
 * ae/svnserver/handler_rev_hashes.ae. The format-line accessors and
 * rep_cache_sec_lookup are all Aether-native; the C version only
 * existed to bridge svnae_repo_secondary_hashes' fixed char[4][32]
 * shape, which the Aether port doesn't need. */

/* svnae_openssl_b64_decode + svnserver_txn_add_b64 retired in
 * Round 110 — the b64 decode + txn-add path now lives entirely
 * in commit_parse.ae's txn_add_b64_ helper, calling
 * svnae_crypto_b64_decode_capture / _len + svnae_txn_add_file
 * directly (the _aether wrapper went away in Round 123 — #297
 * auto-unwrap made it dead weight). */

/* Header value or "" — Aether tests string.length == 0 to distinguish
 * "header absent" from "header present + empty" since string externs
 * can't return NULL. */
const char *svnserver_request_header(HttpRequest *req, const char *name) {
    const char *v = req_header(req, name);
    return (v && *v) ? v : "";
}
/* svnserver_request_branch retired in Round 142 — was a one-line
 * convenience around req_header(req, "Svn-Branch") with a "main"
 * fallback. .ae callers now inline the same two-line check
 * (svnserver_request_header + length check). */
int svnserver_spec_allows(const char *repo, const char *branch,
                           const char *path, int is_super) {
    if (is_super) return 1;
    if (!repo || !branch || !path) return 0;
    return aether_branch_spec_allows(repo, branch, path) == 1;
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
