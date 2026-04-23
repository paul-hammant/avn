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
extern const char *aether_json_escape_string(const char *v);
extern const char *aether_json_int_to_dec(int v);
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

/* Forward declarations — ACL helpers below reference these before
 * they're defined in this file. Full struct sb definition inlined so
 * the ACL pass can use it (moves the shared definition up). */
struct sb { char *data; size_t len, cap; };
static void sb_push(struct sb *s, const char *p, size_t n);
static void sb_putc(struct sb *s, char c);
static void sb_puts(struct sb *s, const char *p);
static void sb_putjson_string(struct sb *s, const char *v);
static void sb_putjson_int(struct sb *s, int v);
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

/* Compute the algorithm's hex digest of `data[0..len]` into `out`.
 * `out` is [65] bytes. Returns hex length or 0. */
/* Consolidated in ae/ffi/openssl/shim.c. */
extern int svnae_openssl_hash_hex_into(const char *algo, const char *data, int len, char *out);

static int
hex_digest(const char *algo, const char *data, int len, char *out)
{
    return svnae_openssl_hash_hex_into(algo, data, len, out);
}

/* Aether-callable ACL + hash wrappers. The redact walker is ported
 * to ae/svnserver/redact.ae, which reaches through these. */
int svnserver_acl_allows(const char *repo, int rev, const char *user,
                         const char *path) {
    return acl_allows(repo, rev, user, path);
}

const char *svnserver_hash_hex(const char *repo, const char *data, int length) {
    static __thread char buf[65];
    buf[0] = '\0';
    const char *algo = svnae_repo_primary_hash(repo);
    hex_digest(algo, data ? data : "", length, buf);
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

/* --- JSON string builder ---------------------------------------------- */

static void sb_push(struct sb *s, const char *p, size_t n)
{
    if (s->len + n + 1 > s->cap) {
        size_t nc = s->cap ? s->cap * 2 : 256;
        while (nc < s->len + n + 1) nc *= 2;
        s->data = realloc(s->data, nc);
        s->cap = nc;
    }
    memcpy(s->data + s->len, p, n);
    s->len += n;
    s->data[s->len] = '\0';
}
static void sb_putc(struct sb *s, char c) { sb_push(s, &c, 1); }
static void sb_puts(struct sb *s, const char *p) { sb_push(s, p, strlen(p)); }

/* JSON formatting ported to Aether (ae/svnserver/json.ae, --emit=lib).
 * The C entry points are now thin wrappers so none of the 35-ish call
 * sites need to change. */
static void
sb_putjson_string(struct sb *s, const char *v)
{
    sb_puts(s, aether_json_escape_string(v ? v : ""));
}

static void
sb_putjson_int(struct sb *s, int v)
{
    sb_puts(s, aether_json_int_to_dec(v));
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

/* GET /repos/{r}/rev/:rev/info  → info_rev JSON
 * GET /repos/{r}/rev/:rev/cat/<path>   → raw bytes
 * GET /repos/{r}/rev/:rev/list/<path>  → list JSON
 * GET /repos/{r}/rev/:rev/props/<path> → props JSON
 *
 * Single handler dispatches based on what follows the rev number.
 */
static void
handle_repo_rev(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail) { respond_error(res, 404, "not found"); return; }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }

    int rev;
    const char *after;
    if (!parse_rev_from_tail(tail, &rev, &after)) {
        respond_error(res, 400, "malformed rev");
        return;
    }

    /* /rev/N/info — ported to ae/svnserver/handler_rev_info.ae. */
    if (strcmp(after, "/info") == 0) {
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        extern void aether_rev_info_handle(void *req, void *res,
                                            const char *repo, int rev,
                                            const char *user, int is_super);
        aether_rev_info_handle(req, res, repo, rev,
                               user ? user : "", is_super);
        return;
    }

    /* /rev/N/cat/<path> — logic in Aether (handler_rev_cat.ae); the
     * final respond_binary stays in C because std.http lacks a
     * length-aware body setter. */
    if (strncmp(after, "/cat/", 5) == 0 || strcmp(after, "/cat") == 0) {
        const char *file_path = strcmp(after, "/cat") == 0 ? "" : after + 5;
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        extern int aether_cat_handle(HttpRequest *req, HttpServerResponse *res,
                                     const char *repo, int rev,
                                     const char *file_path,
                                     const char *user, int is_super);
        (void)aether_cat_handle(req, res, repo, rev, file_path,
                                user ? user : "", is_super);
        return;
    }

    /* /rev/N/paths → {"entries":[{"action":"A","path":"..."}, ...]}
     * ACL-filtered; super sees all. Routing+response now done in
     * ae/svnserver/handler_rev_paths.ae. */
    if (strcmp(after, "/paths") == 0) {
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        extern void aether_paths_handle(void *req, void *res,
                                         const char *repo, int rev,
                                         const char *user, int is_super);
        aether_paths_handle(req, res, repo, rev,
                            user ? user : "", is_super);
        return;
    }

    /* /rev/N/blame/<path> — ported to ae/svnserver/handler_rev_blame.ae. */
    if (strncmp(after, "/blame/", 7) == 0) {
        const char *target = after + 7;
        const char *bl_user = NULL;
        int bl_is_super = auth_context(req, &bl_user);
        extern void aether_blame_handle(void *req, void *res,
                                         const char *repo, int rev,
                                         const char *target,
                                         const char *user, int is_super);
        aether_blame_handle(req, res, repo, rev, target,
                            bl_user ? bl_user : "", bl_is_super);
        return;
    }

    /* /rev/N/hashes/<path> → {"primary":{algo,hash}, "secondaries":[...]}
     * Ported to ae/svnserver/handler_rev_hashes.ae. */
    if (strncmp(after, "/hashes/", 8) == 0 || strcmp(after, "/hashes") == 0) {
        const char *target = strcmp(after, "/hashes") == 0 ? "" : after + 8;
        const char *hu_user = NULL;
        int hu_is_super = auth_context(req, &hu_user);
        extern void aether_hashes_handle(void *req, void *res,
                                          const char *repo, int rev,
                                          const char *target,
                                          const char *user, int is_super);
        aether_hashes_handle(req, res, repo, rev, target,
                             hu_user ? hu_user : "", hu_is_super);
        return;
    }
    /* /rev/N/acl/<path> → ACL lookup + body in Aether. Super-user
     * only; normal users get 404. Ported to ae/svnserver/handler_rev_acl.ae. */
    if (strncmp(after, "/acl/", 5) == 0 || strcmp(after, "/acl") == 0) {
        const char *target = strcmp(after, "/acl") == 0 ? "" : after + 5;
        const char *a_user = NULL;
        int a_is_super = auth_context(req, &a_user);
        extern void aether_acl_handle(void *req, void *res,
                                       const char *repo, int rev,
                                       const char *target,
                                       const char *user, int is_super);
        aether_acl_handle(req, res, repo, rev, target,
                          a_user ? a_user : "", a_is_super);
        return;
    }

    /* /rev/N/props/<path> — ported to ae/svnserver/handler_rev_props.ae. */
    if (strncmp(after, "/props/", 7) == 0 || strcmp(after, "/props") == 0) {
        const char *target = strcmp(after, "/props") == 0 ? "" : after + 7;
        const char *p_user = NULL;
        int p_is_super = auth_context(req, &p_user);
        extern void aether_props_handle(void *req, void *res,
                                         const char *repo, int rev,
                                         const char *target,
                                         const char *user, int is_super);
        aether_props_handle(req, res, repo, rev, target,
                            p_user ? p_user : "", p_is_super);
        return;
    }

    /* /rev/N/list/<path>  (path "" → root)
     *
     * Phase 7.1: may redact per-child entries based on the caller's
     * ACL view. Redacted children appear as {"kind":"hidden",
     * "sha":"<hex>"} — no name. The dir's X-Svnae-Node-Hash header
     * is recomputed against the redacted dir blob so the client's
     * Merkle walk terminates at the same sha the server reports.
     *
     * Super-user: sees the raw dir blob verbatim (no redaction, no
     * rehash — same as Phase 6.2 behaviour). */
    if (strncmp(after, "/list/", 6) == 0 || strcmp(after, "/list") == 0) {
        const char *dir_path = strcmp(after, "/list") == 0 ? "" : after + 6;
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        extern void aether_list_handle(HttpRequest *req, HttpServerResponse *res,
                                       const char *repo, int rev,
                                       const char *dir_path,
                                       const char *user, int is_super);
        aether_list_handle(req, res, repo, rev, dir_path,
                           user ? user : "", is_super);
        return;
    }

    respond_error(res, 404, "not found");
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

/* Optimistic-concurrency check: compare Svn-Based-On header against
 * the current content sha at `path`. Returns:
 *    1 OK (either the caller's based-on matches, or the caller omitted
 *      it and the path currently doesn't exist — "create new")
 *    0 conflict (response already emitted with 409)
 */
static int
based_on_check(HttpRequest *req, HttpServerResponse *res,
               const char *repo, int head_rev, const char *path,
               int allow_missing_as_create)
{
    const char *based_on = header_or_null(req, "Svn-Based-On");

    char cur_sha[65] = {0};
    char cur_kind = 0;
    int exists = svnae_repos_resolve(repo, head_rev, path, cur_sha, &cur_kind);

    if (!based_on) {
        /* Omitted. Caller is asserting "create new": fail if path
         * already exists (unless allow_missing_as_create is 0 — DELETE
         * always needs based_on, even though the path must exist). */
        if (exists && allow_missing_as_create) {
            http_response_set_header(res, "X-Svnae-Current-Hash", cur_sha);
            respond_error(res, 409, "based_on missing but path exists");
            return 0;
        }
        if (!exists && !allow_missing_as_create) {
            respond_error(res, 404, "not found");
            return 0;
        }
        return 1;
    }

    /* Header present. If the path doesn't exist we return 404 —
     * "already gone" is a more actionable signal to scripted callers
     * than a generic based_on mismatch. Distinguishable from an
     * auth-denial 404 by the header being attempted at all. */
    if (!exists) {
        respond_error(res, 404, "not found");
        return 0;
    }
    if (strcmp(based_on, cur_sha) != 0) {
        http_response_set_header(res, "X-Svnae-Current-Hash", cur_sha);
        respond_error(res, 409, "based_on mismatch");
        return 0;
    }
    return 1;
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
int svnserver_branch_create_from_body(const char *repo,
                                       const char *branch_name,
                                       const char *body, int body_len,
                                       const char *user_for_author) {
    if (!body || body_len <= 0) return -1;
    cJSON *root = cJSON_ParseWithLength(body, (size_t)body_len);
    if (!root) return -1;

    cJSON *jbase = cJSON_GetObjectItemCaseSensitive(root, "base");
    cJSON *jinc  = cJSON_GetObjectItemCaseSensitive(root, "include");
    if (!cJSON_IsString(jbase) || !cJSON_IsArray(jinc)) {
        cJSON_Delete(root);
        return -2;
    }
    int n_inc = cJSON_GetArraySize(jinc);
    if (n_inc <= 0) {
        cJSON_Delete(root);
        return -3;
    }

    const char **globs = calloc((size_t)n_inc, sizeof *globs);
    char **glob_copies = calloc((size_t)n_inc, sizeof *glob_copies);
    int gi = 0;
    for (int i = 0; i < n_inc; i++) {
        cJSON *e = cJSON_GetArrayItem(jinc, i);
        if (cJSON_IsString(e) && json_valuestring(e) && *json_valuestring(e)) {
            glob_copies[gi] = strdup(json_valuestring(e));
            globs[gi] = glob_copies[gi];
            gi++;
        }
    }
    char *base_copy = strdup(json_valuestring(jbase));
    cJSON_Delete(root);

    int new_rev = svnae_branch_create(repo, branch_name, base_copy,
                                      globs, gi,
                                      (user_for_author && *user_for_author)
                                        ? user_for_author : "super");

    for (int i = 0; i < gi; i++) free(glob_copies[i]);
    free(glob_copies); free(globs); free(base_copy);

    if (new_rev < 0) return -4;
    return new_rev;
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
int svnserver_commit_from_body(HttpRequest *req, HttpServerResponse *res,
                                const char *repo) {
    (void)res;   /* all error paths return -N; Aether handler emits response */
    if (!req->body || req->body_length == 0) return -1;

    cJSON *root = cJSON_ParseWithLength(req->body, req->body_length);
    if (!root) return -2;

    cJSON *jbase   = cJSON_GetObjectItemCaseSensitive(root, "base_rev");
    cJSON *jauthor = cJSON_GetObjectItemCaseSensitive(root, "author");
    cJSON *jlog    = cJSON_GetObjectItemCaseSensitive(root, "log");
    cJSON *jedits  = cJSON_GetObjectItemCaseSensitive(root, "edits");
    if (!cJSON_IsNumber(jbase) || !cJSON_IsString(jauthor) ||
        !cJSON_IsString(jlog)  || !cJSON_IsArray(jedits)) {
        cJSON_Delete(root);
        return -3;
    }

    int base_rev = json_valueint(jbase);

    /* Write-ACL pre-scan (Phase 7.2): before building the txn, walk
     * every edit path (and every ACL-touched path, below) and confirm
     * the caller has write permission. Super-user bypasses. A denied
     * path yields 403 for the whole commit — we don't partially apply.
     *
     * Phase 8.2b: same scan also enforces the branch include spec.
     * Body may carry "branch": "<name>" (preferred); Svn-Branch header
     * is the fallback. Absent both → "main". */
    const char *commit_branch;
    {
        cJSON *jbranch = cJSON_GetObjectItemCaseSensitive(root, "branch");
        commit_branch = (jbranch && cJSON_IsString(jbranch) && *json_valuestring(jbranch))
                        ? json_valuestring(jbranch)
                        : request_branch(req);
    }
    {
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        cJSON *ck;
        cJSON_ArrayForEach(ck, jedits) {
            cJSON *jp = cJSON_GetObjectItemCaseSensitive(ck, "path");
            if (!cJSON_IsString(jp)) continue;
            const char *epath = json_valuestring(jp);
            if (!is_super &&
                !acl_allows_mode(repo, base_rev, user, epath, 1)) {
                cJSON_Delete(root);
                return -4;
            }
            if (!spec_allows(repo, commit_branch, epath, is_super)) {
                cJSON_Delete(root);
                return -5;
            }
        }
        if (!is_super) {
            /* ACL changes themselves require write on the target path —
             * otherwise a restricted user could self-elevate by setting
             * +alice on /secret. */
            cJSON *jacl_pre = cJSON_GetObjectItemCaseSensitive(root, "acl");
            if (jacl_pre && cJSON_IsObject(jacl_pre)) {
                int pre_n = json_object_size_raw(jacl_pre);
                for (int pi = 0; pi < pre_n; pi++) {
                    const char *apath = json_object_key_at(jacl_pre, pi);
                    if (!apath) apath = "";
                    if (!acl_allows_mode(repo, base_rev, user, apath, 1)) {
                        cJSON_Delete(root);
                        return -4;
                    }
                }
            }
        }
    }

    /* Copy out — we cJSON_Delete before calling finalise. */
    char *author = strdup(json_valuestring(jauthor));
    char *logmsg = strdup(json_valuestring(jlog));

    struct svnae_txn *txn = svnae_txn_new(base_rev);
    if (!txn) { free(author); free(logmsg); cJSON_Delete(root); return -6; }

    cJSON *e;
    cJSON_ArrayForEach(e, jedits) {
        cJSON *jop   = cJSON_GetObjectItemCaseSensitive(e, "op");
        cJSON *jpath = cJSON_GetObjectItemCaseSensitive(e, "path");
        if (!cJSON_IsString(jop) || !cJSON_IsString(jpath)) {
            svnae_txn_free(txn); free(author); free(logmsg); cJSON_Delete(root);
            return -7;
        }
        const char *op   = json_valuestring(jop);
        const char *path = json_valuestring(jpath);

        if (strcmp(op, "add-file") == 0) {
            cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(e, "content");
            if (!cJSON_IsString(jcontent)) {
                svnae_txn_free(txn); free(author); free(logmsg); cJSON_Delete(root);
                return -8;
            }
            const char *b64 = json_valuestring(jcontent);
            unsigned char *raw = NULL;
            int raw_len = 0;
            if (b64_decode(b64, (int)strlen(b64), &raw, &raw_len) != 0) {
                svnae_txn_free(txn); free(author); free(logmsg); cJSON_Delete(root);
                return -9;
            }
            svnae_txn_add_file(txn, path, (const char *)raw, raw_len);
            free(raw);
        } else if (strcmp(op, "mkdir") == 0) {
            svnae_txn_mkdir(txn, path);
        } else if (strcmp(op, "delete") == 0) {
            svnae_txn_delete(txn, path);
        } else {
            svnae_txn_free(txn); free(author); free(logmsg); cJSON_Delete(root);
            return -10;
        }
    }

    /* Optional top-level "props" object: { path: {key: value, ...}, ... } */
    /* The WC always sends ALL current props for the set of paths it wants
     * to persist. We honour what we're given verbatim. Must run BEFORE
     * cJSON_Delete(root) since jprops lives inside root. */
    cJSON *jprops = cJSON_GetObjectItemCaseSensitive(root, "props");
    const char *use_props_sha1 = "";   /* default -> inherit prev */
    char *built_props_sha1 = NULL;
    if (jprops && cJSON_IsObject(jprops)) {
        /* Build paths-props blob. */
        int np = json_object_size_raw(jprops);
        if (np > 0) {
            const char **paths      = calloc((size_t)np, sizeof *paths);
            const char **props_shas = calloc((size_t)np, sizeof *props_shas);
            char       **props_sha_copies = calloc((size_t)np, sizeof *props_sha_copies);
            int i = 0;
            for (int pi = 0; pi < np; pi++) {
                const char *this_path = json_object_key_at(jprops, pi);
                cJSON *p = json_object_value_at(jprops, pi);
                paths[i] = this_path ? this_path : "";
                /* p is an object; its children are key:value string pairs. */
                int nk = cJSON_IsObject(p) ? json_object_size_raw(p) : 0;
                const char **keys   = calloc((size_t)(nk > 0 ? nk : 1), sizeof *keys);
                const char **values = calloc((size_t)(nk > 0 ? nk : 1), sizeof *values);
                int k = 0;
                for (int ki = 0; ki < nk; ki++) {
                    cJSON *kv = json_object_value_at(p, ki);
                    if (cJSON_IsString(kv)) {
                        const char *kname = json_object_key_at(p, ki);
                        keys[k]   = kname ? kname : "";
                        values[k] = json_valuestring(kv) ? json_valuestring(kv) : "";
                        k++;
                    }
                }
                const char *psha = svnae_build_props_blob(repo, keys, values, k);
                free(keys); free(values);
                if (!psha) psha = "";
                props_sha_copies[i] = strdup(psha);
                props_shas[i]       = props_sha_copies[i];
                i++;
            }
            const char *psb = svnae_build_paths_props_blob(repo, paths, props_shas, i);
            if (psb) built_props_sha1 = strdup(psb);
            for (int j = 0; j < i; j++) free(props_sha_copies[j]);
            free(props_sha_copies); free(paths); free(props_shas);
            if (built_props_sha1) use_props_sha1 = built_props_sha1;
        }
    }
    /* Optional top-level "acl" object: { path: ["+alice","-eve"], ... }.
     * Empty array removes the ACL on that path. If the key is omitted
     * entirely, ACLs inherit from the previous rev (handled by
     * svnae_commit_finalise_with_acl when we pass ""). */
    cJSON *jacl = cJSON_GetObjectItemCaseSensitive(root, "acl");
    const char *use_acl_sha1 = "";
    char *built_acl_sha1 = NULL;
    if (jacl && cJSON_IsObject(jacl)) {
        int np = json_object_size_raw(jacl);
        if (np > 0) {
            const char **paths     = calloc((size_t)np, sizeof *paths);
            const char **acl_shas  = calloc((size_t)np, sizeof *acl_shas);
            char       **sha_copies = calloc((size_t)np, sizeof *sha_copies);
            int i = 0;
            for (int pi = 0; pi < np; pi++) {
                const char *this_path = json_object_key_at(jacl, pi);
                cJSON *p = json_object_value_at(jacl, pi);
                if (!cJSON_IsArray(p)) continue;
                int nr = cJSON_GetArraySize(p);
                if (nr == 0) continue;   /* empty = remove */
                const char **rules = calloc((size_t)nr, sizeof *rules);
                int rcount = 0;
                for (int k = 0; k < nr; k++) {
                    cJSON *r = cJSON_GetArrayItem(p, k);
                    if (cJSON_IsString(r)) rules[rcount++] = json_valuestring(r);
                }
                const char *asha = svnae_build_acl_blob(repo, rules, rcount);
                free(rules);
                if (!asha) asha = "";
                paths[i] = this_path ? this_path : "";
                sha_copies[i] = strdup(asha);
                acl_shas[i] = sha_copies[i];
                i++;
            }
            if (i > 0) {
                const char *pab = svnae_build_paths_acl_blob(repo, paths, acl_shas, i);
                if (pab) built_acl_sha1 = strdup(pab);
            }
            for (int j = 0; j < i; j++) free(sha_copies[j]);
            free(sha_copies); free(paths); free(acl_shas);
            if (built_acl_sha1) use_acl_sha1 = built_acl_sha1;
        }
    }
    cJSON_Delete(root);

    int new_rev = svnae_commit_finalise_with_acl(repo, txn, author, logmsg,
                                                 use_props_sha1, use_acl_sha1);
    svnae_txn_free(txn);
    free(author);
    free(logmsg);
    free(built_props_sha1);
    free(built_acl_sha1);
    if (new_rev < 0) return -11;
    return new_rev;
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
int svnserver_copy_from_body(HttpRequest *req, const char *repo) {
    if (!req->body || req->body_length == 0) return -1;
    cJSON *root = cJSON_ParseWithLength(req->body, req->body_length);
    if (!root) return -2;

    cJSON *jbase = cJSON_GetObjectItemCaseSensitive(root, "base_rev");
    cJSON *jfrom = cJSON_GetObjectItemCaseSensitive(root, "from_path");
    cJSON *jto   = cJSON_GetObjectItemCaseSensitive(root, "to_path");
    cJSON *jaut  = cJSON_GetObjectItemCaseSensitive(root, "author");
    cJSON *jlog  = cJSON_GetObjectItemCaseSensitive(root, "log");
    if (!cJSON_IsNumber(jbase) || !cJSON_IsString(jfrom) ||
        !cJSON_IsString(jto)   || !cJSON_IsString(jaut)  ||
        !cJSON_IsString(jlog)) {
        cJSON_Delete(root);
        return -3;
    }

    int base_rev = json_valueint(jbase);
    char *from_path = strdup(json_valuestring(jfrom));
    char *to_path   = strdup(json_valuestring(jto));
    char *author    = strdup(json_valuestring(jaut));
    char *logmsg    = strdup(json_valuestring(jlog));
    cJSON_Delete(root);

    const char *user = NULL;
    int is_super = auth_context(req, &user);
    if (!is_super) {
        if (!acl_user_has_rw_subtree(repo, base_rev, user, from_path) ||
            !acl_allows_mode(repo, base_rev, user, to_path, 1)) {
            free(from_path); free(to_path); free(author); free(logmsg);
            return -4;
        }
    }
    const char *cp_branch = request_branch(req);
    if (!spec_allows(repo, cp_branch, from_path, is_super) ||
        !spec_allows(repo, cp_branch, to_path,   is_super)) {
        free(from_path); free(to_path); free(author); free(logmsg);
        return -5;
    }

    char sha1[65];
    char kind_char;
    if (!svnae_repos_resolve(repo, base_rev, from_path, sha1, &kind_char)) {
        free(from_path); free(to_path); free(author); free(logmsg);
        return -6;
    }

    char *new_acl_sha = auto_follow_copy_acl(repo, base_rev, from_path, to_path);

    struct svnae_txn *txn = svnae_txn_new(base_rev);
    svnae_txn_copy(txn, to_path, sha1, kind_char == 'd' ? 1 : 0);
    int new_rev = svnae_commit_finalise_with_acl(repo, txn, author, logmsg,
                                                  "",
                                                  new_acl_sha ? new_acl_sha : "");
    svnae_txn_free(txn);
    free(from_path); free(to_path); free(author); free(logmsg);
    free(new_acl_sha);

    if (new_rev < 0) return -7;
    return new_rev;
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

/* Unified dispatcher — examines the path and picks the right handler.
 * Needed because std.http's wildcard is end-of-pattern-greedy, so
 * we can't distinguish sub-routes at the route layer — they all
 * match the same pattern. Dispatcher re-parses the URL tail.
 *
 * GET  /info, /log, /rev/N/*     -> read handlers above
 * POST /commit                    -> handle_repo_commit
 *
 * std.http routes POST and GET separately; we register two routes both
 * pointing at this dispatcher. It looks at req->method to disambiguate. */
static void
dispatch(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail) { respond_error(res, 404, "not found"); return; }

    if (strcmp(req->method, "POST") == 0) {
        if (strcmp(tail, "/commit") == 0) { aether_handler_commit(req, res); return; }
        if (strcmp(tail, "/copy")   == 0) { aether_handler_copy(req, res); return; }

        /* /branches/<NAME>/create — Phase 8.2a. Parse+auth+commit all
         * in ae/svnserver/handler_branch_create.ae. Delegate for any
         * tail starting with "/branches/" and let Aether 404 on
         * malformed shapes. */
        if (strncmp(tail, "/branches/", 10) == 0) {
            aether_handler_branch_create(req, res);
            return;
        }

        respond_error(res, 404, "unknown POST route");
        return;
    }

    /* Phase 7.4: REST node edits. /path/<rel> is the resource URL. */
    if (strcmp(req->method, "PUT") == 0) {
        if (strncmp(tail, "/path/", 6) == 0) { handle_repo_path_put(req, res); return; }
        respond_error(res, 404, "unknown PUT route");
        return;
    }
    if (strcmp(req->method, "DELETE") == 0) {
        if (strncmp(tail, "/path/", 6) == 0) { handle_repo_path_delete(req, res); return; }
        respond_error(res, 404, "unknown DELETE route");
        return;
    }

    if (strncmp(tail, "/path/", 6) == 0) { handle_repo_path_get(req, res); return; }
    if (strcmp(tail, "/info") == 0)      { handle_repo_info(req, res, user_data); return; }
    if (strcmp(tail, "/log") == 0)       { handle_repo_log (req, res, user_data); return; }
    if (strncmp(tail, "/rev/", 5) == 0)  { handle_repo_rev (req, res, user_data); return; }

    respond_error(res, 404, "unknown sub-route");
}

void *svnae_svnserver_handler_dispatch(void) { return (void *)dispatch; }
