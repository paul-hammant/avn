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

static char *load_rev_blob_field(const char *repo, int rev, const char *key);

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

/* Parse a per-path ACL blob body and decide whether `user` is allowed
 * in mode `want_write` (0 = read, 1 = write).
 *
 * Rule syntax:
 *   +alice          allow read+write
 *   +alice:r        allow read only
 *   +alice:w        allow write only
 *   +alice:rw       allow read+write (explicit)
 *   -alice          deny both (modes on deny are ignored — deny is absolute)
 *   +* / -*         wildcard, same grammar
 *
 * Returns 1 allow, 0 deny, -1 no-match. Precedence: explicit user
 * rule beats wildcard; deny beats allow at the same precedence level.
 */
/* ACL rule evaluation + paths-index ancestor walk both ported; the
 * sole remaining user was acl_allows_mode above, which now calls
 * ae/svnserver/acl_mode.ae directly. */

/* Ancestry-walking ACL mode check ported to
 * ae/svnserver/acl_mode.ae::acl_allows_mode. */
extern int aether_acl_allows_mode(const char *repo, int rev,
                                  const char *user, const char *target_path,
                                  int want_write);

extern int svnae_openssl_hash_hex_into(const char *algo, const char *data, int len, char *out);

/* Read-check wrapper — used by list/cat/props/log/paths via the
 * Aether redact walker (ae/svnserver/redact.ae). Thin front for the
 * ported acl_allows_mode (want_write=0). */
int svnserver_acl_allows(const char *repo, int rev, const char *user,
                         const char *path) {
    return aether_acl_allows_mode(repo, rev, user, path, 0);
}

const char *svnserver_hash_hex(const char *repo, const char *data, int length) {
    static __thread char buf[65];
    buf[0] = '\0';
    svnae_openssl_hash_hex_into(svnae_repo_primary_hash(repo),
                                data ? data : "", length, buf);
    return buf;
}

/* load_rev_blob_field is a file-scope static defined further down.
 * Forward-declare + re-export via a stable wrapper for the Aether
 * acl_resolve port. */
static char *load_rev_blob_field(const char *repo, int rev, const char *key);

const char *svnserver_load_rev_acl_root(const char *repo, int rev) {
    static __thread char buf[65];
    buf[0] = '\0';
    char *sha = load_rev_blob_field(repo, rev, "acl");
    if (sha) {
        size_t n = strlen(sha);
        if (n < sizeof buf) { memcpy(buf, sha, n + 1); }
        free(sha);
    }
    return buf;
}

const char *svnserver_load_rev_props_sha(const char *repo, int rev) {
    static __thread char buf[65];
    buf[0] = '\0';
    char *sha = load_rev_blob_field(repo, rev, "props");
    if (sha) {
        size_t n = strlen(sha);
        if (n < sizeof buf) { memcpy(buf, sha, n + 1); }
        free(sha);
    }
    return buf;
}

const char *svnserver_load_rev_root_sha(const char *repo, int rev) {
    static __thread char buf[65];
    buf[0] = '\0';
    char *sha = load_rev_blob_field(repo, rev, "root");
    if (sha) {
        size_t n = strlen(sha);
        if (n < sizeof buf) { memcpy(buf, sha, n + 1); }
        free(sha);
    }
    return buf;
}

/* Figure out the effective auth context for this request. Returns
 * 1 if super-user (bypass ACL), 0 otherwise. Writes the effective
 * username (possibly "") into `*out_user` as a static-thread-local. */
static int
auth_context(HttpRequest *req, const char **out_user)
{
    static __thread char user_cache[128];
    const char *hdr_user  = req_header(req, "X-Svnae-User");
    const char *hdr_super = req_header(req, "X-Svnae-Superuser");
    if (hdr_user) {
        size_t n = strlen(hdr_user);
        if (n >= sizeof user_cache) n = sizeof user_cache - 1;
        memcpy(user_cache, hdr_user, n);
        user_cache[n] = '\0';
    } else {
        user_cache[0] = '\0';   /* anonymous */
    }
    *out_user = user_cache;
    if (hdr_super && g_superuser_token && strcmp(hdr_super, g_superuser_token) == 0) {
        return 1;
    }
    return 0;
}

/* Aether-callable split of auth_context. The Aether port can't
 * handle out-params across FFI gracefully, so we expose two probes. */
const char *svnserver_request_user(HttpRequest *req) {
    const char *user = NULL;
    (void)auth_context(req, &user);
    return user ? user : "";
}

int svnserver_request_is_super(HttpRequest *req) {
    const char *user = NULL;
    return auth_context(req, &user);
}

/* sb_* JSON-string-builder helpers previously lived here; all call
 * sites have moved to Aether-side builders in ae/svnserver/json.ae
 * and friends. Dead code removed. */

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

static const char *
find_repo_path(const char *name)
{
    if (!g_repo_name || !g_repo_path) return NULL;
    return strcmp(name, g_repo_name) == 0 ? g_repo_path : NULL;
}

/* Aether-callable wrapper. Returns "" for missing (Aether can't
 * return NULL from a string extern). */
const char *svnserver_find_repo_path(const char *name) {
    const char *p = find_repo_path(name);
    return p ? p : "";
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

/* URL parsing, the dispatcher, and individual route handlers all
 * moved to Aether (dispatch.ae, handle_repo_rev.ae, handler_*.ae);
 * the C-side trampolines have been removed.
 *
 * What remains in this file: small wrappers that expose static C
 * helpers to Aether (request header lookup, auth context), the
 * repo-name map, respond_* utilities bound to std.http, and the
 * three cJSON-heavy body-action wrappers for branch_create / copy /
 * commit. Everything else in svnserver/ is Aether-side. */

/* Ported to ae/svnserver/rev_load.ae. The Aether version returns
 * "" on miss instead of NULL; adapt at the boundary so existing
 * C callers keep their `if (!acl_root) ...` idiom. */
extern const char *aether_load_rev_blob_field(const char *repo, int rev,
                                              const char *key);
static char *
load_rev_blob_field(const char *repo, int rev, const char *key)
{
    const char *v = aether_load_rev_blob_field(repo, rev, key);
    if (!v || !*v) return NULL;
    return strdup(v);
}

/* load_rev_props_sha1 / paths_props_lookup / sb_put_props_as_json
 * were the building blocks of the /rev/N/props handler; all moved
 * to Aether (ae/svnserver/acl_resolve.ae::props_resolve) and
 * ae/svnserver/json.ae. */

/* --- registration entry points --------------------------------------- *
 *
 * The Aether orchestration calls these to hand back handler function
 * pointers that it then passes to http_server_add_route / _get.
 *
 * Each returns a void* so Aether's ptr type system can carry it.
 */

/* Base64 decode consolidated in ae/ffi/openssl/shim.c. */
extern int svnae_openssl_b64_decode(const char *src, int src_len,
                                    unsigned char **out, int *out_len);
#define b64_decode svnae_openssl_b64_decode

/* --- REST node edit handlers (Phase 7.4) -----------------------------
 *
 *   GET    /repos/{r}/path/<path>  → raw bytes at HEAD (+ Merkle hdrs)
 *   PUT    /repos/{r}/path/<path>  → update existing file or create new
 *   DELETE /repos/{r}/path/<path>  → remove file/dir from HEAD
 *
 * Required / optional headers on PUT and DELETE:
 *   Svn-Based-On: <hex>  — prior content sha of this path. Omitted =
 *                          "path must not currently exist" (PUT only).
 *                          On mismatch: 409 + X-Svnae-Current-Hash.
 *   Svn-Author:   <name> — commit author; defaults to X-Svnae-User.
 *   Svn-Log:      <msg>  — commit log message; default is synthesized.
 *   X-Svnae-User: <name> — placeholder auth, as with other endpoints.
 *   X-Svnae-Superuser: <token> — bypass ACL + optimistic-concurrency.
 */

static const char *
header_or_null(HttpRequest *req, const char *name)
{
    const char *v = req_header(req, name);
    return (v && *v) ? v : NULL;
}

/* Phase 8.2b: pick the caller's branch for a mutation. Clients pass
 * Svn-Branch: <name> when they're working against a non-main branch;
 * absent header defaults to "main" (which on seeded repos with no
 * spec means "allow everything", preserving legacy behaviour). */
static const char *
request_branch(HttpRequest *req)
{
    const char *b = header_or_null(req, "Svn-Branch");
    return (b && *b) ? b : "main";
}

/* Wrapper that respects super-user bypass: super always allowed, non-super
 * is checked against the branch's include spec. Returns 1 = allow. */
static int
spec_allows(const char *repo, const char *branch, const char *path, int is_super)
{
    if (is_super) return 1;
    return svnae_branch_spec_allows(repo, branch, path) == 1;
}

/* Optimistic-concurrency check ported to
 * ae/svnserver/based_on_check.ae. */
extern int aether_based_on_check(void *req, void *res,
                                 const char *repo, int head_rev,
                                 const char *path,
                                 int allow_missing_as_create);

/* Aether-callable wrappers around the static request-inspection and
 * mutation-policy helpers above. Each returns "" for a NULL string so
 * Aether's string externs can distinguish "header absent" from
 * "header present + empty" by string.length == 0. based_on_check
 * emits its own 4xx response on failure; we just propagate the int. */
const char *svnserver_request_header(HttpRequest *req, const char *name) {
    const char *v = req_header(req, name);
    return (v && *v) ? v : "";
}
const char *svnserver_request_branch(HttpRequest *req) {
    return request_branch(req);
}
int svnserver_acl_allows_write(const char *repo, int rev,
                                const char *user, const char *path) {
    return aether_acl_allows_mode(repo, rev, user, path, 1);
}
/* Both-mode wrapper. Aether's acl_subtree walker needs to check
 * RO and RW separately on the same path, so it's cleaner to take
 * `want_write` as an argument than to expose two wrappers. */
int svnserver_acl_allows_mode(const char *repo, int rev,
                               const char *user, const char *path,
                               int want_write) {
    return aether_acl_allows_mode(repo, rev, user, path, want_write);
}
int svnserver_spec_allows(const char *repo, const char *branch,
                           const char *path, int is_super) {
    return spec_allows(repo, branch, path, is_super);
}
int svnserver_based_on_check(HttpRequest *req, HttpServerResponse *res,
                              const char *repo, int head_rev,
                              const char *path,
                              int allow_missing_as_create) {
    return aether_based_on_check(req, res, repo, head_rev, path,
                                 allow_missing_as_create);
}

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

/* svnserver_branch_create_globs used to split the newline-joined glob
 * string Aether built back into a char** array for svnae_branch_create,
 * which then rejoined it back to pass to aether_filter_dir. Absurd
 * round trip; svnae_branch_create now takes globs_joined directly and
 * the Aether branch-create handler calls it without any C wrapper. */

/* svnserver_branch_create_from_body C trampoline removed — the
 * Aether handler calls aether_branch_create_from_body directly. */

/* --- commit handler --------------------------------------------------- *
 *
 *   POST /repos/{r}/commit
 *   Body: {
 *     "base_rev": N,
 *     "author":   "...",
 *     "log":      "...",
 *     "edits": [
 *       {"op": "add-file", "path": "...", "content": "<base64>"},
 *       {"op": "mkdir",    "path": "..."},
 *       {"op": "delete",   "path": "..."}
 *     ]
 *   }
 *   → 200 {"rev": N+1}
 *   → 400 on malformed body
 *   → 500 on txn rebuild failure
 */

/* Parse the commit JSON body, run ACL/spec pre-scan, build a txn,
 * build paths-props and paths-acl blobs if included, and
 * commit-finalise-with-acl. Return value encodes the outcome:
 *    >= 0 : new_rev
 *    -1   : empty body
 *    -2   : malformed JSON
 *    -3   : missing/wrong-typed top-level field
 *    -4   : forbidden (ACL write denied OR acl-change without write)
 *    -5   : path outside branch spec
 *    -6   : oom building txn
 *    -7   : edit missing op/path
 *    -8   : add-file missing content
 *    -9   : base64 decode failed
 *    -10  : unknown op
 *    -11  : commit finalise failed
 * The Aether handler maps each to the right HTTP status. */
/* svnserver_commit_from_body C trampoline removed — the Aether
 * handler calls aether_commit_from_body directly. */

/* POST /commit — ported to ae/svnserver/handler_commit.ae; dispatcher
 * binds the Aether handler directly. */

/* Subtree RW check ported to ae/svnserver/acl_subtree.ae; called
 * directly from ae/svnserver/copy_parse.ae as
 * aether_acl_user_has_rw_subtree. */

/* Collect (path, acl-sha) pairs from the base rev's paths-acl blob
 * where the path starts with `from_path` (or equals it). For each
 * match, emit a parallel (rebased_path, acl-sha) pair under
 * `to_path`. Returns a paths-acl blob sha for the new rev, or NULL
 * if no entries needed rewriting.
 *
 * This is how `svn cp` auto-follows ACLs: the branch inherits the
 * source's restrictions verbatim, so visibility for anyone other
 * than the caller doesn't change.
 *
 * Caller frees via free() on the returned sha. */

/* End-to-end auto-follow ported to ae/svnserver/copy_acl.ae::
 * auto_follow_copy_acl. Returns "" on miss; adapt at the
 * boundary so the copy handler's existing NULL check keeps
 * working. */
extern const char *aether_auto_follow_copy_acl(const char *repo, int base_rev,
                                               const char *from_path,
                                               const char *to_path);
static char *
auto_follow_copy_acl(const char *repo, int base_rev,
                    const char *from_path, const char *to_path)
{
    const char *v = aether_auto_follow_copy_acl(repo, base_rev, from_path, to_path);
    if (!v || !*v) return NULL;
    return strdup(v);
}

/* --- copy handler ------------------------------------------------------ *
 *
 *   POST /repos/{r}/copy
 *   Body: { "base_rev": N, "from_path": "...", "to_path": "...",
 *           "author": "...", "log": "..." }
 *
 * Atomic server-side copy — the new revision's tree contains to_path
 * pointing at the exact sha1 of from_path@base_rev. Full rep-sharing.
 * Caller gets back {"rev": N+1}.
 */
/* Parse the copy JSON body, ACL/spec-check, resolve from, auto-follow
 * ACLs, txn-copy, commit. Return value encodes the outcome:
 *    >= 0 : new_rev
 *    -1   : empty body
 *    -2   : malformed JSON
 *    -3   : missing/wrong-typed field
 *    -4   : forbidden (non-super lacks RW on from subtree or to)
 *    -5   : cross-branch copy refused
 *    -6   : from_path missing at base_rev
 *    -7   : commit failed
 * The Aether handler maps each to an HTTP response code. */
/* svnserver_copy_from_body C trampoline removed — the Aether
 * handler calls aether_copy_from_body directly. */

/* POST /copy — ported to ae/svnserver/handler_copy.ae; dispatcher
 * binds the Aether handler directly. */

/* Phase 8.2a: POST /repos/{r}/branches/<NAME>/create
 *
 *   Body: { "base": "main", "include": ["src/**", "README.md"] }
 *
 * Creates a new branch rooted on the given base branch, filtered
 * through the include globs. Super-user only (branch creation is
 * privileged by default; future phases may add branch-level ACLs).
 */

/* POST /branches/<NAME>/create — ported to ae/svnserver/handler_branch_create.ae.
 * Aether parses the branch name from the URL itself, so the dispatcher
 * no longer needs to split it out. The dispatcher binds the Aether
 * handler directly. */

/* svnae_svnserver_handler_{info,log,rev} used to expose C-side
 * function pointers to an earlier Aether orchestration pass. Both
 * the trampolines and their exporters have been dead since the
 * full dispatcher moved to Aether; dropped. svnae_svnserver_handler_dispatch
 * is the only survivor — it hands std.http's route registration a
 * pointer at the Aether dispatch entry. */
extern void aether_svnserver_dispatch(void *req, void *res, void *user_data);

void *svnae_svnserver_handler_dispatch(void) { return (void *)aether_svnserver_dispatch; }
