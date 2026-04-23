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

#include "aether_json.h"

/* cJSON → std.json compatibility layer (see ae/ra/shim.c for the full
 * design note). Only the parse/read side is needed here. */
typedef JsonValue cJSON;
#define cJSON_ParseWithLength(b, n)            json_parse_raw_n((b), (n))
#define cJSON_Delete(v)                        json_free(v)
#define cJSON_GetObjectItemCaseSensitive(o, k) json_object_get_raw((o), (k))
#define cJSON_IsNumber(v)                      ((v) && json_type(v) == JSON_NUMBER)
#define cJSON_IsString(v)                      ((v) && json_type(v) == JSON_STRING)
#define cJSON_IsArray(v)                       ((v) && json_type(v) == JSON_ARRAY)
#define cJSON_IsObject(v)                      ((v) && json_type(v) == JSON_OBJECT)
#define cJSON_GetArraySize(a)                  json_array_size(a)
#define cJSON_GetArrayItem(a, i)               json_array_get_raw((a), (i))
static inline int         json_valueint(JsonValue *v)    { return json_get_int(v); }
static inline const char *json_valuestring(JsonValue *v) { return json_get_string_raw(v); }
#define cJSON_ArrayForEach(entry, arr)                                 \
    for (int _sa_idx = 0,                                              \
             _sa_len = json_array_size(arr);                           \
         _sa_idx < _sa_len &&                                          \
         ((entry) = json_array_get_raw((arr), _sa_idx), 1);            \
         _sa_idx++)
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* --- forward decls from other shims ------------------------------------ */

struct svnae_log;
struct svnae_list;
struct svnae_info;
struct svnae_txn;

struct svnae_txn *svnae_txn_new(int base_rev);
int  svnae_txn_add_file(struct svnae_txn *t, const char *path, const char *content, int len);
int  svnae_txn_mkdir(struct svnae_txn *t, const char *path);
int  svnae_txn_delete(struct svnae_txn *t, const char *path);
int  svnae_txn_copy  (struct svnae_txn *t, const char *path, const char *from_sha1, int from_kind);
void svnae_txn_free(struct svnae_txn *t);

/* Phase 8.2b enforcement. */
int  svnae_branch_spec_allows(const char *repo, const char *branch, const char *path);

/* Dir-blob line parser ported to Aether (ae/fs_fs/dirblob.ae). */
extern int         aether_dir_count_entries(const char *body);
extern int         aether_dir_entry_kind(const char *body, int i);
extern const char *aether_dir_entry_sha(const char *body, int i);
extern const char *aether_dir_entry_name(const char *body, int i);

/* All other aether_* externs used throughout. Consolidated here so
 * every call site (including respond_error which lives very early) has
 * the declarations in scope. */
extern const char *aether_props_blob_to_json(const char *body);
extern const char *aether_specs_to_json_array(const char *body);
extern const char *aether_log_entry_json(int rev, const char *author, const char *date, const char *msg);
extern const char *aether_path_change_entry_json(const char *action, const char *path);
extern const char *aether_blame_entry_json(int rev, const char *author, const char *text);
extern const char *aether_list_entry_visible_json(const char *name, int kind_c);
extern const char *aether_list_entry_hidden_json(const char *sha);
extern const char *aether_redact_line_visible(int kind_c, const char *sha, const char *name);
extern const char *aether_redact_line_hidden(const char *sha);
extern const char *aether_info_prelude_json(int head, const char *name, const char *hash_algo);
extern const char *aether_rev_info_json(int rev, const char *author, const char *date, const char *msg, const char *root);
extern const char *aether_error_response_json(const char *msg);
extern const char *aether_rev_response_json(int rev);
extern const char *aether_rev_sha_response_json(int rev, const char *sha);
extern const char *aether_rev_branch_response_json(int rev, const char *branch);
extern const char *aether_hashes_prelude_json(const char *algo, const char *primary_hash);
extern const char *aether_secondary_entry_json(const char *algo, const char *hash);
extern const char *aether_acl_response_json(const char *rules_body, const char *effective_from);
extern const char *aether_copy_acl_follow(const char *body, const char *from_path, const char *to_path);
extern const char *aether_paths_index_sort_by_path(const char *body);

int  svnae_commit_finalise(const char *repo, struct svnae_txn *txn,
                           const char *author, const char *logmsg);
int  svnae_commit_finalise_with_props(const char *repo, struct svnae_txn *txn,
                                      const char *author, const char *logmsg,
                                      const char *props_sha1);
const char *svnae_build_props_blob(const char *repo,
                                   const char *const *keys,
                                   const char *const *values,
                                   int n_pairs);
const char *svnae_build_paths_props_blob(const char *repo,
                                         const char *const *paths,
                                         const char *const *props_shas,
                                         int n_paths);
int  svnae_commit_finalise_with_acl(const char *repo, struct svnae_txn *txn,
                                    const char *author, const char *logmsg,
                                    const char *props_sha1, const char *acl_sha1);
const char *svnae_build_acl_blob(const char *repo,
                                 const char *const *rules, int n_rules);
const char *svnae_build_paths_acl_blob(const char *repo,
                                       const char *const *paths,
                                       const char *const *acl_shas,
                                       int n_paths);

/* For server-side copy: resolve a (rev, path) pair to its sha1 + kind.
 * We piggy-back on the existing repos/shim.c's resolve_path by exposing
 * a small helper there. */
int  svnae_repos_resolve(const char *repo, int rev, const char *path,
                         char *out_sha1, char *out_kind /* 'f' or 'd' */);

int          svnae_repos_head_rev(const char *repo);

struct svnae_log  *svnae_repos_log(const char *repo);
int               svnae_repos_log_count(const struct svnae_log *lg);
int               svnae_repos_log_rev(const struct svnae_log *lg, int i);
const char       *svnae_repos_log_author(const struct svnae_log *lg, int i);
const char       *svnae_repos_log_date(const struct svnae_log *lg, int i);
const char       *svnae_repos_log_msg(const struct svnae_log *lg, int i);
void              svnae_repos_log_free(struct svnae_log *lg);

char             *svnae_repos_cat(const char *repo, int rev, const char *path);
char             *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
void              svnae_rep_free(char *p);
const char       *svnae_repo_primary_hash(const char *repo);
int                svnae_repo_secondary_hashes(const char *repo, char out[4][32]);
char              *svnae_rep_lookup_secondary(const char *repo,
                                              const char *primary_hex,
                                              const char *algo);

struct svnae_blame *svnae_repos_blame(const char *repo, int rev, const char *path);
int         svnae_blame_count (const struct svnae_blame *B);
int         svnae_blame_rev   (const struct svnae_blame *B, int i);
const char *svnae_blame_author(const struct svnae_blame *B, int i);
const char *svnae_blame_text  (const struct svnae_blame *B, int i);
void        svnae_blame_free  (struct svnae_blame *B);

struct svnae_list *svnae_repos_list(const char *repo, int rev, const char *path);
int               svnae_repos_list_count(const struct svnae_list *L);
const char       *svnae_repos_list_name(const struct svnae_list *L, int i);
const char       *svnae_repos_list_kind(const struct svnae_list *L, int i);
void              svnae_repos_list_free(struct svnae_list *L);

struct svnae_info *svnae_repos_info_rev(const char *repo, int rev);
int               svnae_repos_info_rev_num(const struct svnae_info *I);
const char       *svnae_repos_info_author(const struct svnae_info *I);
const char       *svnae_repos_info_date(const struct svnae_info *I);
const char       *svnae_repos_info_msg(const struct svnae_info *I);
void              svnae_repos_info_free(struct svnae_info *I);

struct svnae_paths *svnae_repos_paths_changed(const char *repo, int rev);
int                 svnae_repos_paths_count(const struct svnae_paths *P);
const char         *svnae_repos_paths_path(const struct svnae_paths *P, int i);
const char         *svnae_repos_paths_action(const struct svnae_paths *P, int i);
void                svnae_repos_paths_free(struct svnae_paths *P);

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

/* We need to set a binary body (for cat). std.http exposes set_body which
 * strdup's + strlen's — no good. Stuff the fields directly. */
static void
respond_binary(HttpServerResponse *res, const char *data, size_t len, const char *content_type)
{
    http_response_set_status(res, 200);
    http_response_set_header(res, "Content-Type", content_type);

    /* Free any prior body set via http_response_set_body. */
    free(res->body);
    res->body = malloc(len + 1);
    if (res->body) {
        memcpy(res->body, data, len);
        res->body[len] = '\0';          /* safe sentinel; real length is body_length */
        res->body_length = len;
    } else {
        res->body_length = 0;
    }
    char lenbuf[32];
    snprintf(lenbuf, sizeof lenbuf, "%zu", len);
    http_response_set_header(res, "Content-Length", lenbuf);
}

static void
respond_json(HttpServerResponse *res, int code, const char *json)
{
    http_response_set_status(res, code);
    http_response_json(res, json);
}

static void
respond_error(HttpServerResponse *res, int code, const char *message)
{
    respond_json(res, code, aether_error_response_json(message ? message : ""));
}

/* Attach Merkle-verification headers describing the node being
 * returned by this response. Callers that know the node's kind and
 * content sha use this; callers that don't (errors, untyped JSON)
 * leave them off. The headers advertise:
 *   X-Svnae-Hash-Algo  — the repo's primary hash name (e.g. sha256)
 *   X-Svnae-Node-Kind  — "file" or "dir"
 *   X-Svnae-Node-Hash  — hex sha of this node's content blob
 *
 * Clients use these to independently re-hash fetched bytes and build
 * a Merkle proof back to the revision's root sha. */
static void
set_merkle_headers(HttpServerResponse *res, const char *algo,
                   const char *kind, const char *sha)
{
    if (algo && *algo)
        http_response_set_header(res, "X-Svnae-Hash-Algo", algo);
    if (kind && *kind)
        http_response_set_header(res, "X-Svnae-Node-Kind", kind);
    if (sha && *sha)
        http_response_set_header(res, "X-Svnae-Node-Hash", sha);
}

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

/* Case-insensitive header lookup on an HttpRequest. The Aether stdlib
 * doesn't expose header arrays, so we rely on fields added via Phase 7.1.
 * For now we parse them from `req->query_string` if absent (fallback).
 * TODO: plumb through a proper headers array when std.http exposes one. */
typedef struct {
    char** keys;
    char** values;
    int    count;
} __HdrPair;

/* Aether http_server request type already declared above; header arrays
 * live on it as header_keys / header_values / header_count (mirroring
 * the response struct). */
static const char *
req_header(HttpRequest *req, const char *name)
{
    for (int i = 0; i < req->header_count; i++) {
        if (strcasecmp(req->header_keys[i], name) == 0) return req->header_values[i];
    }
    return NULL;
}

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
static int
acl_allows_mode(const char *repo, int rev, const char *user,
                const char *target_path, int want_write)
{
    return aether_acl_allows_mode(repo, rev, user, target_path, want_write);
}

/* Back-compat read-check wrapper — used by list/cat/props/log/paths. */
static int
acl_allows(const char *repo, int rev, const char *user, const char *target_path)
{
    return acl_allows_mode(repo, rev, user, target_path, 0);
}

extern int svnae_openssl_hash_hex_into(const char *algo, const char *data, int len, char *out);

/* Aether-callable ACL + hash wrappers. The redact walker is ported
 * to ae/svnserver/redact.ae, which reaches through these. */
int svnserver_acl_allows(const char *repo, int rev, const char *user,
                         const char *path) {
    return acl_allows(repo, rev, user, path);
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

/* Recursively compute the redacted-for-`user` sha of the dir at
 * `dir_sha`. Writes 65-byte hex into `out`. Returns 0 on success.
 * Ported to Aether (ae/svnserver/redact.ae); the wrapper preserves
 * the old out-param signature for the three call sites. */
extern const char *aether_redacted_dir_sha(const char *repo, int rev,
                                           const char *user,
                                           const char *dir_sha,
                                           const char *prefix);

static int
compute_redacted_dir_sha(const char *repo, int rev, const char *user,
                         const char *dir_sha, const char *prefix, char *out)
{
    const char *hex = aether_redacted_dir_sha(repo, rev, user, dir_sha, prefix);
    if (!hex || !*hex) { out[0] = '\0'; return -1; }
    size_t n = strlen(hex);
    if (n >= 65) n = 64;
    memcpy(out, hex, n);
    out[n] = '\0';
    return 0;
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

/* Aether-callable passthroughs to the static helpers defined earlier
 * in the file (respond_error / respond_binary / set_merkle_headers). */
void svnserver_respond_error(HttpServerResponse *res, int code, const char *msg) {
    respond_error(res, code, msg);
}

void svnserver_respond_binary_ok(HttpServerResponse *res, const char *data,
                                 int length, const char *content_type) {
    respond_binary(res, data, (size_t)length, content_type);
}

void svnserver_set_merkle_headers(HttpServerResponse *res, const char *algo,
                                  const char *kind, const char *sha) {
    set_merkle_headers(res, algo, kind, sha);
}

void svnserver_respond_json_ok(HttpServerResponse *res, const char *body) {
    respond_json(res, 200, body);
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

/* --- path parsing helpers --------------------------------------------- *
 *
 * URL shape:  /repos/{R}/...
 * After matching {R} we have the tail starting with /
 *
 * For the sub-routes we parse manually since std.http's path-param support
 * doesn't cover the "capture rest of path" pattern we need for cat/list.
 */

/* Given "/repos/NAME/tail", populate name (up to sizeof buf) and return
 * pointer to the tail (starting with '/'), or NULL on malformed input. */
static const char *
parse_repo_and_tail(const char *path, char *name_buf, size_t name_sz)
{
    const char *prefix = "/repos/";
    size_t plen = strlen(prefix);
    if (strncmp(path, prefix, plen) != 0) return NULL;
    const char *p = path + plen;
    const char *slash = strchr(p, '/');
    size_t nlen = slash ? (size_t)(slash - p) : strlen(p);
    if (nlen == 0 || nlen >= name_sz) return NULL;
    memcpy(name_buf, p, nlen);
    name_buf[nlen] = '\0';
    return slash ? slash : p + nlen;  /* if no slash, return empty-string tail */
}

/* --- handlers --------------------------------------------------------- */

/* Ported to Aether (ae/svnserver/url_parse.ae). The thin wrapper keeps
 * the original out-param shape the two call sites use. */
extern int aether_parse_rev_prefix(const char *tail);
extern int aether_parse_rev_prefix_end(const char *tail);

static int
parse_rev_from_tail(const char *tail, int *out_rev, const char **after)
{
    int end = aether_parse_rev_prefix_end(tail);
    if (end < 0) return 0;
    *out_rev = aether_parse_rev_prefix(tail);
    *after = tail + end;   /* points at '/', or '\0' */
    return 1;
}

/* GET /repos/{r}/info — fully in Aether (ae/svnserver/handler_info.ae).
 * The C wrapper only exists so the dispatch table keeps pointing at a
 * stable C function. */
extern void aether_handler_info(HttpRequest *req, HttpServerResponse *res);

static void
handle_repo_info(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    aether_handler_info(req, res);
}

/* GET /repos/{r}/log — fully in Aether (ae/svnserver/handler_log.ae). */
extern void aether_handler_log(HttpRequest *req, HttpServerResponse *res);

static void
handle_repo_log(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    aether_handler_log(req, res);
}

/* Field extraction ported to Aether (ae/repos/blobfield.ae). The C side
 * keeps the rev-pointer + rep-blob I/O. */
extern const char *aether_blobfield_get(const char *body, const char *key);

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

/* GET /repos/{r}/rev/N/<sub> dispatcher ported to
 * ae/svnserver/handle_repo_rev.ae. */
extern void aether_handle_repo_rev(void *req, void *res);
static void
handle_repo_rev(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    aether_handle_repo_rev(req, res);
}

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

/* GET /repos/{r}/path/<path> — ported to ae/svnserver/handler_path_get.ae. */
extern void aether_handler_path_get(void *req, void *res);
static void
handle_repo_path_get(HttpRequest *req, HttpServerResponse *res)
{
    aether_handler_path_get(req, res);
}

/* Optimistic-concurrency check ported to
 * ae/svnserver/based_on_check.ae. */
extern int aether_based_on_check(void *req, void *res,
                                 const char *repo, int head_rev,
                                 const char *path,
                                 int allow_missing_as_create);
static int
based_on_check(HttpRequest *req, HttpServerResponse *res,
               const char *repo, int head_rev, const char *path,
               int allow_missing_as_create)
{
    return aether_based_on_check(req, res, repo, head_rev, path,
                                 allow_missing_as_create);
}

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
    return acl_allows_mode(repo, rev, user, path, 1);
}
/* Both-mode wrapper. Aether's acl_subtree walker needs to check
 * RO and RW separately on the same path, so it's cleaner to take
 * `want_write` as an argument than to expose two wrappers. */
int svnserver_acl_allows_mode(const char *repo, int rev,
                               const char *user, const char *path,
                               int want_write) {
    return acl_allows_mode(repo, rev, user, path, want_write);
}
int svnserver_spec_allows(const char *repo, const char *branch,
                           const char *path, int is_super) {
    return spec_allows(repo, branch, path, is_super);
}
int svnserver_based_on_check(HttpRequest *req, HttpServerResponse *res,
                              const char *repo, int head_rev,
                              const char *path,
                              int allow_missing_as_create) {
    return based_on_check(req, res, repo, head_rev, path,
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

/* Count newlines + 1 (or 0 if empty). Shared helper. */
static int count_lines(const char *joined) {
    if (!joined || !*joined) return 0;
    int n = 1;
    for (const char *q = joined; *q; q++) if (*q == '\n') n++;
    return n;
}

/* Split a \n-joined string into a strdup'd array of line pieces.
 * Caller frees each copy and the array. Returns the number of
 * lines populated. Trailing empty line (from final \n) is skipped. */
static int split_joined(const char *joined, char ***out_pieces) {
    int n_lines = count_lines(joined);
    char **pieces = calloc((size_t)(n_lines > 0 ? n_lines : 1), sizeof *pieces);
    int gi = 0;
    const char *p = joined ? joined : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t slen = eol ? (size_t)(eol - p) : strlen(p);
        pieces[gi++] = strndup(p, slen);   /* empty lines preserved */
        if (!eol) break;
        p = eol + 1;
    }
    *out_pieces = pieces;
    return gi;
}

static void free_joined_split(char **pieces, int n) {
    for (int i = 0; i < n; i++) free(pieces[i]);
    free(pieces);
}

/* Build a props blob from \n-joined (keys, values). Caller frees
 * via svnae_rep_free. "" on failure. */
const char *svnserver_build_props_blob_joined(const char *repo,
                                               const char *keys_joined,
                                               const char *values_joined) {
    char **keys = NULL, **values = NULL;
    int nk = split_joined(keys_joined, &keys);
    int nv = split_joined(values_joined, &values);
    int n = nk < nv ? nk : nv;
    const char **kp = calloc((size_t)(n > 0 ? n : 1), sizeof *kp);
    const char **vp = calloc((size_t)(n > 0 ? n : 1), sizeof *vp);
    for (int i = 0; i < n; i++) { kp[i] = keys[i]; vp[i] = values[i]; }
    const char *sha = svnae_build_props_blob(repo, kp, vp, n);
    free(kp); free(vp);
    free_joined_split(keys, nk);
    free_joined_split(values, nv);
    return sha ? sha : "";
}

/* Build a paths-props blob from \n-joined (paths, props_shas). */
const char *svnserver_build_paths_props_blob_joined(const char *repo,
                                                     const char *paths_joined,
                                                     const char *shas_joined) {
    char **paths = NULL, **shas = NULL;
    int np = split_joined(paths_joined, &paths);
    int ns = split_joined(shas_joined, &shas);
    int n = np < ns ? np : ns;
    const char **pp = calloc((size_t)(n > 0 ? n : 1), sizeof *pp);
    const char **sp = calloc((size_t)(n > 0 ? n : 1), sizeof *sp);
    for (int i = 0; i < n; i++) { pp[i] = paths[i]; sp[i] = shas[i]; }
    const char *sha = svnae_build_paths_props_blob(repo, pp, sp, n);
    free(pp); free(sp);
    free_joined_split(paths, np);
    free_joined_split(shas, ns);
    return sha ? sha : "";
}

/* Build an ACL rule blob from \n-joined rules. */
const char *svnserver_build_acl_blob_joined(const char *repo,
                                             const char *rules_joined) {
    char **rules = NULL;
    int n = split_joined(rules_joined, &rules);
    const char **rp = calloc((size_t)(n > 0 ? n : 1), sizeof *rp);
    for (int i = 0; i < n; i++) rp[i] = rules[i];
    const char *sha = svnae_build_acl_blob(repo, rp, n);
    free(rp);
    free_joined_split(rules, n);
    return sha ? sha : "";
}

/* Build a paths-acl blob from \n-joined (paths, acl_shas). */
const char *svnserver_build_paths_acl_blob_joined(const char *repo,
                                                   const char *paths_joined,
                                                   const char *shas_joined) {
    char **paths = NULL, **shas = NULL;
    int np = split_joined(paths_joined, &paths);
    int ns = split_joined(shas_joined, &shas);
    int n = np < ns ? np : ns;
    const char **pp = calloc((size_t)(n > 0 ? n : 1), sizeof *pp);
    const char **sp = calloc((size_t)(n > 0 ? n : 1), sizeof *sp);
    for (int i = 0; i < n; i++) { pp[i] = paths[i]; sp[i] = shas[i]; }
    const char *sha = svnae_build_paths_acl_blob(repo, pp, sp, n);
    free(pp); free(sp);
    free_joined_split(paths, np);
    free_joined_split(shas, ns);
    return sha ? sha : "";
}

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

/* Thin C wrapper: Aether builds `globs_joined` as newline-separated
 * glob strings; we split into a const char** array for the existing
 * svnae_branch_create FFI. */
int svnserver_branch_create_globs(const char *repo, const char *branch_name,
                                   const char *base, const char *globs_joined,
                                   const char *author) {
    /* Count newlines + 1 for a tight upper bound. */
    int n = 1;
    for (const char *q = globs_joined; *q; q++) if (*q == '\n') n++;
    const char **globs = calloc((size_t)(n > 0 ? n : 1), sizeof *globs);
    char **copies = calloc((size_t)(n > 0 ? n : 1), sizeof *copies);
    int gi = 0;
    const char *p = globs_joined;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t slen = eol ? (size_t)(eol - p) : strlen(p);
        if (slen > 0) {
            copies[gi] = strndup(p, slen);
            globs[gi] = copies[gi];
            gi++;
        }
        if (!eol) break;
        p = eol + 1;
    }
    int new_rev = svnae_branch_create(repo, branch_name, base, globs, gi,
                                      (author && *author) ? author : "super");
    for (int i = 0; i < gi; i++) free(copies[i]);
    free(copies); free(globs);
    return new_rev;
}
extern int aether_branch_create_from_body(const char *repo,
                                          const char *branch_name,
                                          const char *body, int body_len,
                                          const char *user_for_author);
int svnserver_branch_create_from_body(const char *repo,
                                       const char *branch_name,
                                       const char *body, int body_len,
                                       const char *user_for_author) {
    return aether_branch_create_from_body(repo, branch_name, body, body_len,
                                          user_for_author);
}

/* PUT /repos/{r}/path/<path> — ported to ae/svnserver/handler_path_put.ae. */
extern void aether_handler_path_put(void *req, void *res);
static void
handle_repo_path_put(HttpRequest *req, HttpServerResponse *res)
{
    aether_handler_path_put(req, res);
}

/* DELETE /repos/{r}/path/<path> — ported to ae/svnserver/handler_path_delete.ae. */
extern void aether_handler_path_delete(void *req, void *res);
static void
handle_repo_path_delete(HttpRequest *req, HttpServerResponse *res)
{
    aether_handler_path_delete(req, res);
}

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
extern int aether_commit_from_body(HttpRequest *req, HttpServerResponse *res,
                                   const char *repo);
int svnserver_commit_from_body(HttpRequest *req, HttpServerResponse *res,
                                const char *repo) {
    return aether_commit_from_body(req, res, repo);
}

/* POST /commit — ported to ae/svnserver/handler_commit.ae. */
extern void aether_handler_commit(void *req, void *res);

/* Subtree RW check ported to ae/svnserver/acl_subtree.ae. */
extern int aether_acl_user_has_rw_subtree(const char *repo, int rev,
                                          const char *user, const char *path);
static int
acl_user_has_rw_subtree(const char *repo, int rev, const char *user,
                        const char *path)
{
    return aether_acl_user_has_rw_subtree(repo, rev, user, path);
}

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
extern const char *svnae_build_paths_acl_blob(const char *repo,
                                              const char *const *paths,
                                              const char *const *acl_shas,
                                              int n_paths);

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
extern int aether_copy_from_body(HttpRequest *req, const char *repo);
int svnserver_copy_from_body(HttpRequest *req, const char *repo) {
    return aether_copy_from_body(req, repo);
}

/* POST /copy — ported to ae/svnserver/handler_copy.ae. */
extern void aether_handler_copy(void *req, void *res);

/* Phase 8.2a: POST /repos/{r}/branches/<NAME>/create
 *
 *   Body: { "base": "main", "include": ["src/**", "README.md"] }
 *
 * Creates a new branch rooted on the given base branch, filtered
 * through the include globs. Super-user only (branch creation is
 * privileged by default; future phases may add branch-level ACLs).
 */
extern int svnae_branch_create(const char *repo, const char *name,
                               const char *base,
                               const char *const *globs, int n_globs,
                               const char *author);

/* POST /branches/<NAME>/create — ported to ae/svnserver/handler_branch_create.ae.
 * Aether parses the branch name from the URL itself, so the dispatcher
 * no longer needs to split it out. */
extern void aether_handler_branch_create(void *req, void *res);

void *svnae_svnserver_handler_info(void) { return (void *)handle_repo_info; }
void *svnae_svnserver_handler_log (void) { return (void *)handle_repo_log;  }
void *svnae_svnserver_handler_rev (void) { return (void *)handle_repo_rev;  }

/* Top-level dispatcher ported to ae/svnserver/dispatch.ae. It routes
 * (method, URL-tail) to the already-ported handlers directly. */
extern void aether_svnserver_dispatch(void *req, void *res, void *user_data);

void *svnae_svnserver_handler_dispatch(void) { return (void *)aether_svnserver_dispatch; }
