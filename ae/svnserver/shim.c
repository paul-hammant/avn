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

#include <cjson/cJSON.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <openssl/evp.h>
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
    char buf[256];
    snprintf(buf, sizeof buf, "{\"error\":\"%s\"}", message);
    respond_json(res, code, buf);
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
/* ACL rule evaluation ported to Aether (ae/svnserver/acl.ae). */
extern int aether_acl_decide(const char *body, const char *user, int want_write);

static int
acl_body_decide(const char *body, const char *user, int want_write)
{
    if (!body || !user) return -1;
    return aether_acl_decide(body, user, want_write);
}

/* Paths-index lookup ported to Aether (ae/svnserver/paths_index.ae).
 * Handles either sha1 (40) or sha256 (64) blobs without hardcoding a
 * fixed width. */
extern const char *aether_paths_index_lookup(const char *body, const char *path);

static char *
paths_acl_lookup(const char *body, const char *path)
{
    if (!body) return NULL;
    const char *v = aether_paths_index_lookup(body, path);
    return (v && *v) ? strdup(v) : NULL;
}

/* Decide whether `user` is allowed on `target_path` at `rev` in mode
 * `want_write` (0=read, 1=write). Walks path upward through the
 * paths-acl blob; nearest match wins. Empty path = root. Open by
 * default if no rule applies anywhere in the ancestry. */
static int
acl_allows_mode(const char *repo, int rev, const char *user,
                const char *target_path, int want_write)
{
    char *acl_root = load_rev_blob_field(repo, rev, "acl");
    if (!acl_root || !*acl_root) { free(acl_root); return 1; }
    char *paths_body = svnae_rep_read_blob(repo, acl_root);
    free(acl_root);
    if (!paths_body) return 1;

    char buf[PATH_MAX];
    size_t n = strlen(target_path);
    if (n >= sizeof buf) { svnae_rep_free(paths_body); return 1; }
    memcpy(buf, target_path, n + 1);

    for (;;) {
        char *acl_sha = paths_acl_lookup(paths_body, buf);
        if (acl_sha) {
            char *rules = svnae_rep_read_blob(repo, acl_sha);
            free(acl_sha);
            if (rules) {
                int d = acl_body_decide(rules, user, want_write);
                svnae_rep_free(rules);
                if (d == 0) { svnae_rep_free(paths_body); return 0; }
                if (d == 1) { svnae_rep_free(paths_body); return 1; }
            }
        }
        if (!*buf) break;
        char *last = strrchr(buf, '/');
        if (last) *last = '\0';
        else      buf[0] = '\0';
    }
    svnae_rep_free(paths_body);
    return 1;
}

/* Back-compat read-check wrapper — used by list/cat/props/log/paths. */
static int
acl_allows(const char *repo, int rev, const char *user, const char *target_path)
{
    return acl_allows_mode(repo, rev, user, target_path, 0);
}

/* Compute the algorithm's hex digest of `data[0..len]` into `out`.
 * `out` is [65] bytes. Returns hex length or 0. */
static int
hex_digest(const char *algo, const char *data, int len, char *out)
{
    const EVP_MD *md = NULL;
    if      (strcmp(algo, "sha1")   == 0) md = EVP_sha1();
    else if (strcmp(algo, "sha256") == 0) md = EVP_sha256();
    if (!md) { out[0] = '\0'; return 0; }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char dig[EVP_MAX_MD_SIZE]; unsigned int dlen = 0;
    EVP_DigestInit_ex(ctx, md, NULL);
    EVP_DigestUpdate(ctx, data, (size_t)len);
    EVP_DigestFinal_ex(ctx, dig, &dlen);
    EVP_MD_CTX_free(ctx);
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < dlen; i++) {
        out[i * 2]     = hex[dig[i] >> 4];
        out[i * 2 + 1] = hex[dig[i] & 0x0f];
    }
    out[dlen * 2] = '\0';
    return (int)dlen * 2;
}

/* Recursively compute the redacted-for-`user` sha of the dir at
 * `dir_sha`. Writes 65-byte hex into `out`. Returns 0 on success.
 * `prefix` is the repo-relative path of this dir. */
static int
compute_redacted_dir_sha(const char *repo, int rev, const char *user,
                         const char *dir_sha, const char *prefix, char *out)
{
    char *body = svnae_rep_read_blob(repo, dir_sha);
    if (!body) return -1;
    const char *algo = svnae_repo_primary_hash(repo);
    struct sb redact = {0};
    int n_entries = aether_dir_count_entries(body);
    for (int ei = 0; ei < n_entries; ei++) {
        char kind_c = (char)aether_dir_entry_kind(body, ei);
        /* Snapshot the child fields: recursive compute_redacted_dir_sha
         * below will reuse the Aether thread-local return buffers. */
        char child_sha[65], child_name[PATH_MAX];
        const char *sha_ref = aether_dir_entry_sha(body, ei);
        size_t sha_len = strlen(sha_ref);
        if (sha_len >= sizeof child_sha) continue;
        memcpy(child_sha, sha_ref, sha_len + 1);
        const char *name_ref = aether_dir_entry_name(body, ei);
        size_t name_len = strlen(name_ref);
        if (name_len >= sizeof child_name) continue;
        memcpy(child_name, name_ref, name_len + 1);

        char child_path[PATH_MAX];
        if (*prefix) snprintf(child_path, sizeof child_path, "%s/%s",
                              prefix, child_name);
        else         snprintf(child_path, sizeof child_path, "%s",
                              child_name);
        int allowed = acl_allows(repo, rev, user, child_path);
        if (allowed) {
            if (kind_c == 'd') {
                char sub[65];
                if (compute_redacted_dir_sha(repo, rev, user,
                                             child_sha, child_path, sub) == 0) {
                    char line[PATH_MAX + 96];
                    snprintf(line, sizeof line, "%c %s %s\n",
                             kind_c, sub, child_name);
                    sb_puts(&redact, line);
                }
            } else {
                char line[PATH_MAX + 96];
                snprintf(line, sizeof line, "%c %s %s\n",
                         kind_c, child_sha, child_name);
                sb_puts(&redact, line);
            }
        } else {
            char line[96];
            snprintf(line, sizeof line, "H %s\n", child_sha);
            sb_puts(&redact, line);
        }
    }
    svnae_rep_free(body);
    hex_digest(algo, redact.data ? redact.data : "",
               redact.data ? (int)redact.len : 0, out);
    free(redact.data);
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
extern const char *aether_json_escape_string(const char *v);
extern const char *aether_json_int_to_dec(int v);
extern const char *aether_props_blob_to_json(const char *body);
extern const char *aether_specs_to_json_array(const char *body);
extern const char *aether_log_entry_json(int rev, const char *author, const char *date, const char *msg);
extern const char *aether_path_change_entry_json(const char *action, const char *path);
extern const char *aether_blame_entry_json(int rev, const char *author, const char *text);
extern const char *aether_info_prelude_json(int head, const char *name, const char *hash_algo);
extern const char *aether_rev_info_json(int rev, const char *author, const char *date, const char *msg, const char *root);
extern const char *aether_hashes_prelude_json(const char *algo, const char *primary_hash);
extern const char *aether_secondary_entry_json(const char *algo, const char *hash);

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

static int
parse_rev_from_tail(const char *tail, int *out_rev, const char **after)
{
    /* tail starts with "/rev/" + digits */
    if (strncmp(tail, "/rev/", 5) != 0) return 0;
    const char *p = tail + 5;
    if (!*p || (*p < '0' || *p > '9')) return 0;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out_rev = v;
    *after = p;   /* points at '/', or '\0' */
    return 1;
}

/* GET /repos/{r}/info */
static void
handle_repo_info(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail || strcmp(tail, "/info") != 0) {
        respond_error(res, 404, "not found");
        return;
    }
    const char *repo = find_repo_path(name);
    if (!repo) {
        respond_error(res, 404, "no such repo");
        return;
    }
    int head = svnae_repos_head_rev(repo);
    if (head < 0) {
        respond_error(res, 500, "cannot read head");
        return;
    }
    /* Enumerate branches by listing $repo/branches/. Phase 8.1 has
     * only `main`, but the enumeration is future-proof. */
    char branches_dir[PATH_MAX];
    snprintf(branches_dir, sizeof branches_dir, "%s/branches", repo);
    struct sb s = {0};
    sb_puts(&s, aether_info_prelude_json(head, name, svnae_repo_primary_hash(repo)));
    sb_puts(&s, ",\"branches\":[");
    {
        /* `main` is always implicit in every repo, even ones seeded
         * before the per-branch layout existed. We emit it first and
         * suppress a duplicate if the on-disk dir also has a
         * branches/main entry. */
        sb_putjson_string(&s, "main");
        DIR *d = opendir(branches_dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                if (strcmp(de->d_name, "main") == 0) continue;
                sb_putc(&s, ',');
                sb_putjson_string(&s, de->d_name);
            }
            closedir(d);
        }
    }
    sb_puts(&s, "],\"specs\":{");
    {
        /* Per-branch include globs. main has no spec (full tree)
         * unless explicitly set — we emit [] for it in that case. */
        int first = 1;
        DIR *d = opendir(branches_dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (de->d_name[0] == '.') continue;
                char spec_path[PATH_MAX];
                snprintf(spec_path, sizeof spec_path, "%s/%s/spec",
                         branches_dir, de->d_name);
                FILE *sf = fopen(spec_path, "r");
                if (!first) sb_putc(&s, ',');
                first = 0;
                sb_putjson_string(&s, de->d_name);
                sb_puts(&s, ":[");
                if (sf) {
                    /* Slurp the whole (small) spec file and hand it to
                     * the Aether specs_to_json_array transform, which
                     * handles the line-split + trim + escape + array
                     * framing. We re-open the brackets here so the
                     * helper can own both the "[" and the "]". */
                    fseek(sf, 0, SEEK_END);
                    long spec_sz = ftell(sf);
                    fseek(sf, 0, SEEK_SET);
                    if (spec_sz > 0 && spec_sz < 65536) {
                        char *spec_buf = malloc((size_t)spec_sz + 1);
                        if (spec_buf) {
                            size_t rd = fread(spec_buf, 1, (size_t)spec_sz, sf);
                            spec_buf[rd] = '\0';
                            /* Strip the leading '[' we already emitted
                             * (Aether helper emits its own brackets). */
                            sb_puts(&s, aether_specs_to_json_array(spec_buf) + 1);
                            free(spec_buf);
                            /* aether_specs_to_json_array emits trailing ']'
                             * so we don't need our own. */
                            fclose(sf);
                            continue;
                        }
                    }
                    fclose(sf);
                }
                sb_puts(&s, "]");
            }
            closedir(d);
        }
        /* If main wasn't already listed (no on-disk dir), emit [] for it. */
        char main_spec[PATH_MAX];
        snprintf(main_spec, sizeof main_spec, "%s/main", branches_dir);
        struct stat mst;
        if (stat(main_spec, &mst) != 0) {
            if (!first) sb_putc(&s, ',');
            sb_puts(&s, "\"main\":[]");
        }
    }
    sb_puts(&s, "}");
    sb_puts(&s, "}");
    respond_json(res, 200, s.data ? s.data : "{}");
    free(s.data);
}

/* GET /repos/{r}/log */
static void
handle_repo_log(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail || strcmp(tail, "/log") != 0) {
        respond_error(res, 404, "not found");
        return;
    }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }

    struct svnae_log *lg = svnae_repos_log(repo);
    if (!lg) { respond_error(res, 500, "cannot read log"); return; }

    const char *user = NULL;
    int is_super = auth_context(req, &user);

    struct sb s = {0};
    sb_puts(&s, "{\"entries\":[");
    int n = svnae_repos_log_count(lg);
    int any = 0;
    for (int i = 0; i < n; i++) {
        int rev = svnae_repos_log_rev(lg, i);
        /* A rev is shown to the caller iff at least one of its touched
         * paths is visible. r0 (the empty init commit) has no paths;
         * always show that. Super-user shows everything. */
        int visible = 1;
        if (!is_super && rev > 0) {
            visible = 0;
            struct svnae_paths *P = svnae_repos_paths_changed(repo, rev);
            if (P) {
                int pn = svnae_repos_paths_count(P);
                for (int j = 0; j < pn; j++) {
                    if (acl_allows(repo, rev, user, svnae_repos_paths_path(P, j))) {
                        visible = 1; break;
                    }
                }
                svnae_repos_paths_free(P);
            }
        }
        if (!visible) continue;
        if (any) sb_putc(&s, ',');
        any = 1;
        sb_puts(&s, aether_log_entry_json(rev,
                                          svnae_repos_log_author(lg, i),
                                          svnae_repos_log_date(lg, i),
                                          svnae_repos_log_msg(lg, i)));
    }
    sb_puts(&s, "]}");
    svnae_repos_log_free(lg);
    respond_json(res, 200, s.data ? s.data : "{\"entries\":[]}");
    free(s.data);
}

/* Field extraction ported to Aether (ae/repos/blobfield.ae). The C side
 * keeps the rev-pointer + rep-blob I/O. */
extern const char *aether_blobfield_get(const char *body, const char *key);

static char *
load_rev_blob_field(const char *repo, int rev, const char *key)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/revs/%06d", repo, rev);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[128];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return NULL; }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    char *body = svnae_rep_read_blob(repo, buf);
    if (!body) return NULL;
    const char *v = aether_blobfield_get(body, key);
    char *out = (v && *v) ? strdup(v) : NULL;
    svnae_rep_free(body);
    return out;
}

/* Back-compat: one call site still asks for "props:" specifically. */
static char *
load_rev_props_sha1(const char *repo, int rev)
{
    return load_rev_blob_field(repo, rev, "props");
}

/* Given a paths-props blob body and a target path, return the sha of
 * that path's per-path props blob (malloc'd), or NULL if missing.
 *
 * Shares the paths_acl_lookup Aether port — both blobs share the
 * "<sha> <path>\n" shape and the lookup handles any sha width. */
static char *
paths_props_lookup(const char *body, const char *path)
{
    if (!body) return NULL;
    const char *v = aether_paths_index_lookup(body, path);
    return (v && *v) ? strdup(v) : NULL;
}

/* props-blob → JSON transform ported to Aether (ae/svnserver/json.ae). */
static void
sb_put_props_as_json(struct sb *s, const char *body)
{
    sb_puts(s, aether_props_blob_to_json(body ? body : ""));
}

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

    /* /rev/N/info */
    if (strcmp(after, "/info") == 0) {
        struct svnae_info *I = svnae_repos_info_rev(repo, rev);
        if (!I) { respond_error(res, 404, "no such rev"); return; }

        /* Root sha, possibly redacted if the caller is a non-super-user
         * and any subtree is denied to them. Matters because `svn verify`
         * anchors the Merkle walk here; the user's recomputed root has
         * to equal what /info claims. */
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        char *root_sha = load_rev_blob_field(repo, rev, "root");
        char redacted[65] = {0};
        const char *reported_root = root_sha ? root_sha : "";
        if (root_sha && *root_sha && !is_super) {
            if (compute_redacted_dir_sha(repo, rev, user, root_sha, "", redacted) == 0
                && *redacted) {
                reported_root = redacted;
            }
        }
        const char *body = aether_rev_info_json(svnae_repos_info_rev_num(I),
                                                svnae_repos_info_author(I),
                                                svnae_repos_info_date(I),
                                                svnae_repos_info_msg(I),
                                                reported_root);
        free(root_sha);
        svnae_repos_info_free(I);
        respond_json(res, 200, body ? body : "{}");
        return;
    }

    /* /rev/N/cat/<path>   (path may be empty → root, which is an error for cat) */
    if (strncmp(after, "/cat/", 5) == 0 || strcmp(after, "/cat") == 0) {
        const char *file_path = strcmp(after, "/cat") == 0 ? "" : after + 5;
        if (!*file_path) { respond_error(res, 400, "cat requires a path"); return; }
        /* ACL check before reading: denied = 404 (indistinguishable
         * from "doesn't exist", per Phase 7.1 design). Super-user
         * bypasses. */
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        if (!is_super && !acl_allows(repo, rev, user, file_path)) {
            respond_error(res, 404, "not found");
            return;
        }
        char *data = svnae_repos_cat(repo, rev, file_path);
        if (!data) { respond_error(res, 404, "not found"); return; }
        /* We don't know the length without a separate API call. Our cat
         * returns NUL-terminated text for the test cases; for this Phase 6
         * we use strlen. When binary blobs become common we'll add a
         * length-aware cat variant. */
        size_t len = strlen(data);
        /* Merkle headers: resolve (rev, path) to its content sha. */
        char node_sha[65] = {0};
        char node_kind = 0;
        if (svnae_repos_resolve(repo, rev, file_path, node_sha, &node_kind)) {
            set_merkle_headers(res, svnae_repo_primary_hash(repo),
                               node_kind == 'd' ? "dir" : "file", node_sha);
        }
        respond_binary(res, data, len, "application/octet-stream");
        svnae_rep_free(data);
        return;
    }

    /* /rev/N/paths → {"entries":[{"action":"A","path":"..."}, ...]}
     * Used by `svn log --verbose`. Denied paths are filtered out of
     * the result (indistinguishable from "path didn't change in this
     * rev"). Super-users get the full set. */
    if (strcmp(after, "/paths") == 0) {
        struct svnae_paths *P = svnae_repos_paths_changed(repo, rev);
        if (!P) { respond_error(res, 404, "no such rev"); return; }
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        struct sb s = {0};
        sb_puts(&s, "{\"entries\":[");
        int n = svnae_repos_paths_count(P);
        int any = 0;
        for (int i = 0; i < n; i++) {
            const char *path = svnae_repos_paths_path(P, i);
            if (!is_super && !acl_allows(repo, rev, user, path)) continue;
            if (any) sb_putc(&s, ',');
            any = 1;
            sb_puts(&s, aether_path_change_entry_json(svnae_repos_paths_action(P, i), path));
        }
        sb_puts(&s, "]}");
        svnae_repos_paths_free(P);
        respond_json(res, 200, s.data ? s.data : "{\"entries\":[]}");
        free(s.data);
        return;
    }

    /* /rev/N/blame/<path> → {"lines":[{"rev":..,"author":..,"text":..}]}
     *
     * Per-line attribution at `rev`. ACL applies: denied paths → 404.
     * For very large files this is O(revs * lines^2) on the server;
     * fine for typical source trees, revisit if that becomes a hot
     * spot. */
    if (strncmp(after, "/blame/", 7) == 0) {
        const char *target = after + 7;
        const char *bl_user = NULL;
        int bl_is_super = auth_context(req, &bl_user);
        if (!bl_is_super && !acl_allows(repo, rev, bl_user, target)) {
            respond_error(res, 404, "not found");
            return;
        }

        struct svnae_blame *B = svnae_repos_blame(repo, rev, target);
        if (!B) { respond_error(res, 404, "not found"); return; }

        struct sb s = {0};
        sb_puts(&s, "{\"lines\":[");
        int n = svnae_blame_count(B);
        for (int i = 0; i < n; i++) {
            if (i) sb_putc(&s, ',');
            sb_puts(&s, aether_blame_entry_json(svnae_blame_rev(B, i),
                                                svnae_blame_author(B, i),
                                                svnae_blame_text(B, i)));
        }
        sb_puts(&s, "]}");
        svnae_blame_free(B);
        respond_json(res, 200, s.data ? s.data : "{\"lines\":[]}");
        free(s.data);
        return;
    }

    /* /rev/N/hashes/<path> → {"primary":{algo,hash}, "secondaries":[...]}
     *
     * Used by `svn verify --secondaries`. The client can fetch the
     * content bytes separately (via /cat or /list's child shas), re-
     * hash locally under each algo, and compare with the server's
     * stored secondaries. ACL applies — denied paths return 404. */
    if (strncmp(after, "/hashes/", 8) == 0 || strcmp(after, "/hashes") == 0) {
        const char *target = strcmp(after, "/hashes") == 0 ? "" : after + 8;

        const char *hu_user = NULL;
        int hu_is_super = auth_context(req, &hu_user);
        if (!hu_is_super && !acl_allows(repo, rev, hu_user, target)) {
            respond_error(res, 404, "not found");
            return;
        }

        char node_sha[65] = {0};
        char node_kind = 0;
        if (!svnae_repos_resolve(repo, rev, target, node_sha, &node_kind)) {
            respond_error(res, 404, "not found");
            return;
        }

        const char *algo = svnae_repo_primary_hash(repo);
        struct sb s = {0};
        sb_puts(&s, aether_hashes_prelude_json(algo, node_sha));

        /* Iterate declared secondaries in the format file, looking up
         * each one's stored hash for this blob. Missing entries
         * (e.g. the blob was written before a secondary was added)
         * are silently skipped. */
        char sec[4][32];
        int sec_n = svnae_repo_secondary_hashes(repo, sec);
        int any = 0;
        for (int i = 0; i < sec_n; i++) {
            char *shex = svnae_rep_lookup_secondary(repo, node_sha, sec[i]);
            if (shex && *shex) {
                if (any) sb_putc(&s, ',');
                any = 1;
                sb_puts(&s, aether_secondary_entry_json(sec[i], shex));
            }
            free(shex);
        }
        sb_puts(&s, "]}");
        respond_json(res, 200, s.data ? s.data : "{}");
        free(s.data);
        return;
    }

    /* /rev/N/acl/<path> → {"rules":["+alice","-eve"], "effective_from":"path"}
     *
     * Returns the *effective* ACL at `path` — nearest ancestor wins.
     * Super-user only; normal users that hit this get 404 so they
     * can't probe existence. */
    if (strncmp(after, "/acl/", 5) == 0 || strcmp(after, "/acl") == 0) {
        const char *target = strcmp(after, "/acl") == 0 ? "" : after + 5;
        const char *a_user = NULL;
        int a_is_super = auth_context(req, &a_user);
        if (!a_is_super) { respond_error(res, 404, "not found"); return; }

        /* Walk the paths-acl blob from target upward. */
        char *acl_root = load_rev_blob_field(repo, rev, "acl");
        if (!acl_root || !*acl_root) {
            free(acl_root);
            respond_json(res, 200, "{\"rules\":[],\"effective_from\":\"\"}");
            return;
        }
        char *paths_body = svnae_rep_read_blob(repo, acl_root);
        free(acl_root);
        if (!paths_body) {
            respond_json(res, 200, "{\"rules\":[],\"effective_from\":\"\"}");
            return;
        }
        char buf[PATH_MAX];
        size_t tl = strlen(target);
        if (tl >= sizeof buf) { svnae_rep_free(paths_body); respond_error(res, 400, "path too long"); return; }
        memcpy(buf, target, tl + 1);

        char *eff_path = NULL;
        char *eff_sha = NULL;
        for (;;) {
            eff_sha = paths_acl_lookup(paths_body, buf);
            if (eff_sha) { eff_path = strdup(buf); break; }
            if (!*buf) break;
            char *last = strrchr(buf, '/');
            if (last) *last = '\0';
            else      buf[0] = '\0';
        }
        svnae_rep_free(paths_body);

        struct sb s = {0};
        sb_puts(&s, "{\"rules\":[");
        if (eff_sha) {
            char *rules = svnae_rep_read_blob(repo, eff_sha);
            if (rules) {
                int first = 1;
                const char *p = rules;
                while (*p) {
                    const char *eol = strchr(p, '\n');
                    size_t n = eol ? (size_t)(eol - p) : strlen(p);
                    if (n > 0) {
                        char line[128];
                        if (n < sizeof line) {
                            memcpy(line, p, n); line[n] = '\0';
                            if (!first) sb_putc(&s, ',');
                            sb_putjson_string(&s, line);
                            first = 0;
                        }
                    }
                    if (!eol) break;
                    p = eol + 1;
                }
                svnae_rep_free(rules);
            }
            free(eff_sha);
        }
        sb_puts(&s, "],\"effective_from\":");
        sb_putjson_string(&s, eff_path ? eff_path : "");
        sb_putc(&s, '}');
        free(eff_path);
        respond_json(res, 200, s.data ? s.data : "{}");
        free(s.data);
        return;
    }

    /* /rev/N/props/<path>  */
    if (strncmp(after, "/props/", 7) == 0 || strcmp(after, "/props") == 0) {
        const char *target = strcmp(after, "/props") == 0 ? "" : after + 7;
        /* ACL check: denied paths have empty props from caller's view. */
        const char *p_user = NULL;
        int p_is_super = auth_context(req, &p_user);
        if (!p_is_super && !acl_allows(repo, rev, p_user, target)) {
            respond_json(res, 200, "{}");
            return;
        }
        /* The path "" means the WC root. */
        char *props_sha = load_rev_props_sha1(repo, rev);
        if (!props_sha || !*props_sha) {
            free(props_sha);
            respond_json(res, 200, "{}");
            return;
        }
        char *paths_body = svnae_rep_read_blob(repo, props_sha);
        free(props_sha);
        if (!paths_body) { respond_json(res, 200, "{}"); return; }
        char *per_path_sha = paths_props_lookup(paths_body, target);
        svnae_rep_free(paths_body);
        if (!per_path_sha) { respond_json(res, 200, "{}"); return; }
        char *props_body = svnae_rep_read_blob(repo, per_path_sha);
        if (!props_body) { free(per_path_sha); respond_json(res, 200, "{}"); return; }
        /* Merkle headers: the per-path props blob's sha *is* this
         * node's content hash for verification purposes. */
        set_merkle_headers(res, svnae_repo_primary_hash(repo),
                           "props", per_path_sha);
        free(per_path_sha);
        struct sb s = {0};
        sb_put_props_as_json(&s, props_body);
        svnae_rep_free(props_body);
        respond_json(res, 200, s.data ? s.data : "{}");
        free(s.data);
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

        /* ACL check on the dir itself. If the dir is denied, pretend
         * it doesn't exist. */
        if (!is_super && !acl_allows(repo, rev, user, dir_path)) {
            respond_error(res, 404, "not a directory");
            return;
        }

        /* Resolve the dir's sha + read its raw blob so we can parse
         * children with their content-shas intact. */
        char dir_sha[65] = {0};
        char dir_kind = 0;
        if (!svnae_repos_resolve(repo, rev, dir_path, dir_sha, &dir_kind)
            || dir_kind != 'd') {
            respond_error(res, 404, "not a directory");
            return;
        }
        char *dir_body = svnae_rep_read_blob(repo, dir_sha);
        if (!dir_body) { respond_error(res, 500, "cannot read dir blob"); return; }

        /* Parse each line "<kind> <sha> <name>\n" and decide per-child:
         *   allowed      → emit {name, kind}
         *   denied       → emit {kind:"hidden", sha}, no name
         * Also build the redacted dir blob so we can rehash for the
         * response header. For allowed children the blob line is
         * identical to the original; for denied children it becomes
         * "H <sha>\n" (no trailing space-name, no name).
         *
         * The blob wire format the client needs to recompute this
         * redacted-dir hash is *that* redacted form — so verify knows
         * to rebuild "H <sha>\n" lines for children returned with
         * kind=hidden. */
        struct sb body = {0};     /* JSON output */
        struct sb redact = {0};   /* redacted raw dir blob */
        sb_puts(&body, "{\"entries\":[");
        int any = 0;
        const char *algo = svnae_repo_primary_hash(repo);
        int n_entries = aether_dir_count_entries(dir_body);
        for (int ei = 0; ei < n_entries; ei++) {
            char kind_c = (char)aether_dir_entry_kind(dir_body, ei);
            char child_sha[65], child_name[PATH_MAX];
            const char *sha_ref = aether_dir_entry_sha(dir_body, ei);
            size_t sha_len = strlen(sha_ref);
            if (sha_len >= sizeof child_sha) continue;
            memcpy(child_sha, sha_ref, sha_len + 1);
            const char *name_ref = aether_dir_entry_name(dir_body, ei);
            size_t name_len = strlen(name_ref);
            if (name_len >= sizeof child_name) continue;
            memcpy(child_name, name_ref, name_len + 1);

            char child_path[PATH_MAX];
            if (*dir_path) snprintf(child_path, sizeof child_path, "%s/%s",
                                    dir_path, child_name);
            else           snprintf(child_path, sizeof child_path, "%s",
                                    child_name);

            int allowed = is_super || acl_allows(repo, rev, user, child_path);
            if (any) sb_putc(&body, ',');
            any = 1;
            if (allowed) {
                sb_puts(&body, "{\"name\":");
                sb_putjson_string(&body, child_name);
                sb_puts(&body, ",\"kind\":");
                sb_puts(&body, kind_c == 'd' ? "\"dir\"" : "\"file\"");
                sb_putc(&body, '}');

                /* Preserve the raw line verbatim in the redact buffer. */
                char line[PATH_MAX + 96];
                snprintf(line, sizeof line, "%c %s %s\n",
                         kind_c, child_sha, child_name);
                sb_puts(&redact, line);
            } else {
                sb_puts(&body, "{\"kind\":\"hidden\",\"sha\":");
                sb_putjson_string(&body, child_sha);
                sb_putc(&body, '}');

                /* Redacted form: "H <sha>\n" — no name. */
                char line[96];
                snprintf(line, sizeof line, "H %s\n", child_sha);
                sb_puts(&redact, line);
            }
        }
        sb_puts(&body, "]}");
        svnae_rep_free(dir_body);

        /* Compute the redacted dir's hash (for super-users this
         * collapses to the unmodified dir_sha since nothing was
         * rewritten). */
        if (is_super) {
            set_merkle_headers(res, algo, "dir", dir_sha);
        } else {
            char rehash[65] = {0};
            const EVP_MD *md = NULL;
            if      (strcmp(algo, "sha1")   == 0) md = EVP_sha1();
            else if (strcmp(algo, "sha256") == 0) md = EVP_sha256();
            if (md) {
                EVP_MD_CTX *ctx = EVP_MD_CTX_new();
                unsigned char dig[EVP_MAX_MD_SIZE]; unsigned int dlen = 0;
                EVP_DigestInit_ex(ctx, md, NULL);
                EVP_DigestUpdate(ctx, redact.data ? redact.data : "",
                                 redact.data ? redact.len : 0);
                EVP_DigestFinal_ex(ctx, dig, &dlen);
                EVP_MD_CTX_free(ctx);
                static const char hex[] = "0123456789abcdef";
                for (unsigned int i = 0; i < dlen; i++) {
                    rehash[i*2]   = hex[dig[i] >> 4];
                    rehash[i*2+1] = hex[dig[i] & 0x0f];
                }
                rehash[dlen*2] = '\0';
            }
            set_merkle_headers(res, algo, "dir", rehash);
        }

        free(redact.data);
        respond_json(res, 200, body.data ? body.data : "{\"entries\":[]}");
        free(body.data);
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

/* --- base64 decode (OpenSSL) -----------------------------------------
 *
 * For the commit endpoint, file content travels as base64 inside JSON.
 * OpenSSL's EVP_DecodeBlock is the simplest one-shot decoder. It writes
 * at most 3n/4 bytes for n input bytes (minus padding). */

static int
b64_decode(const char *src, int src_len, unsigned char **out, int *out_len)
{
    /* EVP_DecodeBlock pads output; strip trailing '=' to compute true length. */
    int raw_len = src_len;
    int pad = 0;
    while (raw_len > 0 && src[raw_len - 1] == '=') { raw_len--; pad++; }
    int expected = (src_len / 4) * 3 - pad;
    unsigned char *buf = malloc((size_t)expected + 1);
    if (!buf) return -1;
    int n = EVP_DecodeBlock(buf, (const unsigned char *)src, src_len);
    if (n < 0) { free(buf); return -1; }
    /* EVP_DecodeBlock's returned length includes padding-bytes as zeros;
     * the real length is n - pad. */
    n -= pad;
    if (n < 0) n = 0;
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

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

/* GET /repos/{r}/path/<path>: convenience — returns HEAD content.
 * Internally a redirect-in-code to the /rev/HEAD/cat handler's logic.
 * We reimplement inline rather than fabricate a fake req because the
 * cat handler parses URL tails. */
static void
handle_repo_path_get(HttpRequest *req, HttpServerResponse *res)
{
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail || strncmp(tail, "/path/", 6) != 0) {
        respond_error(res, 404, "not found"); return;
    }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }
    const char *file_path = tail + 6;

    int rev = svnae_repos_head_rev(repo);
    if (rev < 0) { respond_error(res, 500, "cannot read head"); return; }

    const char *user = NULL;
    int is_super = auth_context(req, &user);
    if (!is_super && !acl_allows(repo, rev, user, file_path)) {
        respond_error(res, 404, "not found");
        return;
    }

    char *data = svnae_repos_cat(repo, rev, file_path);
    if (!data) { respond_error(res, 404, "not found"); return; }
    size_t len = strlen(data);
    char node_sha[65] = {0};
    char node_kind = 0;
    if (svnae_repos_resolve(repo, rev, file_path, node_sha, &node_kind)) {
        set_merkle_headers(res, svnae_repo_primary_hash(repo),
                           node_kind == 'd' ? "dir" : "file", node_sha);
    }
    respond_binary(res, data, len, "application/octet-stream");
    svnae_rep_free(data);
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

/* PUT /repos/{r}/path/<path>: replace or create a file with the body
 * bytes. Single-edit atomic commit. */
static void
handle_repo_path_put(HttpRequest *req, HttpServerResponse *res)
{
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail || strncmp(tail, "/path/", 6) != 0) {
        respond_error(res, 404, "not found"); return;
    }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }
    const char *file_path = tail + 6;
    if (!*file_path) { respond_error(res, 400, "empty path"); return; }

    int base_rev = svnae_repos_head_rev(repo);
    if (base_rev < 0) { respond_error(res, 500, "cannot read head"); return; }

    /* Auth + write-ACL. Super-user bypasses. */
    const char *user = NULL;
    int is_super = auth_context(req, &user);
    if (!is_super && !acl_allows_mode(repo, base_rev, user, file_path, 1)) {
        respond_error(res, 403, "forbidden");
        return;
    }

    /* Phase 8.2b: path must fall inside the branch's include spec. */
    if (!spec_allows(repo, request_branch(req), file_path, is_super)) {
        respond_error(res, 403, "path outside branch spec");
        return;
    }

    /* Concurrency check. Super-user also respects based-on — it's
     * lighter than ACL, and skipping it would make scripting unsafe
     * in ways super-users might not expect. */
    if (!based_on_check(req, res, repo, base_rev, file_path, 1)) return;

    /* Build a one-edit txn: add-file with request body as content. */
    struct svnae_txn *txn = svnae_txn_new(base_rev);
    if (!txn) { respond_error(res, 500, "oom"); return; }
    const char *body = req->body ? req->body : "";
    int blen = req->body ? (int)req->body_length : 0;
    svnae_txn_add_file(txn, file_path, body, blen);

    const char *hdr_author = header_or_null(req, "Svn-Author");
    if (!hdr_author) hdr_author = (user && *user) ? user : "anonymous";
    const char *hdr_log = header_or_null(req, "Svn-Log");
    char synth_log[PATH_MAX + 64];
    if (!hdr_log) {
        snprintf(synth_log, sizeof synth_log, "PUT /%s", file_path);
        hdr_log = synth_log;
    }

    int new_rev = svnae_commit_finalise(repo, txn, hdr_author, hdr_log);
    svnae_txn_free(txn);
    if (new_rev < 0) {
        respond_error(res, 500, "commit failed");
        return;
    }

    /* Resolve the new path's sha for the response body. */
    char new_sha[65] = {0};
    char new_kind = 0;
    svnae_repos_resolve(repo, new_rev, file_path, new_sha, &new_kind);

    char buf[256];
    snprintf(buf, sizeof buf, "{\"rev\":%d,\"sha\":\"%s\"}", new_rev, new_sha);
    http_response_set_status(res, 201);
    http_response_json(res, buf);
}

/* DELETE /repos/{r}/path/<path>: remove file or subtree from HEAD.
 * Atomic single-edit commit. */
static void
handle_repo_path_delete(HttpRequest *req, HttpServerResponse *res)
{
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail || strncmp(tail, "/path/", 6) != 0) {
        respond_error(res, 404, "not found"); return;
    }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }
    const char *file_path = tail + 6;
    if (!*file_path) { respond_error(res, 400, "empty path"); return; }

    int base_rev = svnae_repos_head_rev(repo);
    if (base_rev < 0) { respond_error(res, 500, "cannot read head"); return; }

    const char *user = NULL;
    int is_super = auth_context(req, &user);
    if (!is_super && !acl_allows_mode(repo, base_rev, user, file_path, 1)) {
        respond_error(res, 403, "forbidden");
        return;
    }

    /* Phase 8.2b spec check. */
    if (!spec_allows(repo, request_branch(req), file_path, is_super)) {
        respond_error(res, 403, "path outside branch spec");
        return;
    }

    /* DELETE requires the path to exist — "create-if-absent" semantics
     * don't apply. The 4th arg 0 forces the based_on_check to 404 on
     * missing path. */
    if (!based_on_check(req, res, repo, base_rev, file_path, 0)) return;

    struct svnae_txn *txn = svnae_txn_new(base_rev);
    if (!txn) { respond_error(res, 500, "oom"); return; }
    svnae_txn_delete(txn, file_path);

    const char *hdr_author = header_or_null(req, "Svn-Author");
    if (!hdr_author) hdr_author = (user && *user) ? user : "anonymous";
    const char *hdr_log = header_or_null(req, "Svn-Log");
    char synth_log[PATH_MAX + 64];
    if (!hdr_log) {
        snprintf(synth_log, sizeof synth_log, "DELETE /%s", file_path);
        hdr_log = synth_log;
    }

    int new_rev = svnae_commit_finalise(repo, txn, hdr_author, hdr_log);
    svnae_txn_free(txn);
    if (new_rev < 0) {
        respond_error(res, 500, "commit failed");
        return;
    }

    char buf[64];
    snprintf(buf, sizeof buf, "{\"rev\":%d}", new_rev);
    respond_json(res, 200, buf);
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

static void
handle_repo_commit(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail || strcmp(tail, "/commit") != 0) {
        respond_error(res, 404, "not found");
        return;
    }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }

    if (!req->body || req->body_length == 0) {
        respond_error(res, 400, "empty body");
        return;
    }

    cJSON *root = cJSON_ParseWithLength(req->body, req->body_length);
    if (!root) { respond_error(res, 400, "malformed JSON"); return; }

    cJSON *jbase   = cJSON_GetObjectItemCaseSensitive(root, "base_rev");
    cJSON *jauthor = cJSON_GetObjectItemCaseSensitive(root, "author");
    cJSON *jlog    = cJSON_GetObjectItemCaseSensitive(root, "log");
    cJSON *jedits  = cJSON_GetObjectItemCaseSensitive(root, "edits");
    if (!cJSON_IsNumber(jbase) || !cJSON_IsString(jauthor) ||
        !cJSON_IsString(jlog)  || !cJSON_IsArray(jedits)) {
        cJSON_Delete(root);
        respond_error(res, 400, "missing or wrong-typed field");
        return;
    }

    int base_rev = jbase->valueint;

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
        commit_branch = (jbranch && cJSON_IsString(jbranch) && *jbranch->valuestring)
                        ? jbranch->valuestring
                        : request_branch(req);
    }
    {
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        cJSON *ck;
        cJSON_ArrayForEach(ck, jedits) {
            cJSON *jp = cJSON_GetObjectItemCaseSensitive(ck, "path");
            if (!cJSON_IsString(jp)) continue;
            const char *epath = jp->valuestring;
            if (!is_super &&
                !acl_allows_mode(repo, base_rev, user, epath, 1)) {
                cJSON_Delete(root);
                respond_error(res, 403, "forbidden");
                return;
            }
            if (!spec_allows(repo, commit_branch, epath, is_super)) {
                cJSON_Delete(root);
                respond_error(res, 403, "path outside branch spec");
                return;
            }
        }
        if (!is_super) {
            /* ACL changes themselves require write on the target path —
             * otherwise a restricted user could self-elevate by setting
             * +alice on /secret. */
            cJSON *jacl_pre = cJSON_GetObjectItemCaseSensitive(root, "acl");
            if (jacl_pre && cJSON_IsObject(jacl_pre)) {
                for (cJSON *p = jacl_pre->child; p; p = p->next) {
                    const char *apath = p->string ? p->string : "";
                    if (!acl_allows_mode(repo, base_rev, user, apath, 1)) {
                        cJSON_Delete(root);
                        respond_error(res, 403, "forbidden");
                        return;
                    }
                }
            }
        }
    }

    /* Copy out — we cJSON_Delete before calling finalise. */
    char *author = strdup(jauthor->valuestring);
    char *logmsg = strdup(jlog->valuestring);

    struct svnae_txn *txn = svnae_txn_new(base_rev);
    if (!txn) { cJSON_Delete(root); respond_error(res, 500, "oom"); return; }

    cJSON *e;
    cJSON_ArrayForEach(e, jedits) {
        cJSON *jop   = cJSON_GetObjectItemCaseSensitive(e, "op");
        cJSON *jpath = cJSON_GetObjectItemCaseSensitive(e, "path");
        if (!cJSON_IsString(jop) || !cJSON_IsString(jpath)) {
            svnae_txn_free(txn); cJSON_Delete(root);
            respond_error(res, 400, "edit missing op/path");
            return;
        }
        const char *op   = jop->valuestring;
        const char *path = jpath->valuestring;

        if (strcmp(op, "add-file") == 0) {
            cJSON *jcontent = cJSON_GetObjectItemCaseSensitive(e, "content");
            if (!cJSON_IsString(jcontent)) {
                svnae_txn_free(txn); cJSON_Delete(root);
                respond_error(res, 400, "add-file missing content");
                return;
            }
            const char *b64 = jcontent->valuestring;
            unsigned char *raw = NULL;
            int raw_len = 0;
            if (b64_decode(b64, (int)strlen(b64), &raw, &raw_len) != 0) {
                svnae_txn_free(txn); cJSON_Delete(root);
                respond_error(res, 400, "base64 decode failed");
                return;
            }
            svnae_txn_add_file(txn, path, (const char *)raw, raw_len);
            free(raw);
        } else if (strcmp(op, "mkdir") == 0) {
            svnae_txn_mkdir(txn, path);
        } else if (strcmp(op, "delete") == 0) {
            svnae_txn_delete(txn, path);
        } else {
            svnae_txn_free(txn); cJSON_Delete(root);
            respond_error(res, 400, "unknown op");
            return;
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
        int np = 0;
        for (cJSON *it = jprops->child; it; it = it->next) np++;
        if (np > 0) {
            const char **paths      = calloc((size_t)np, sizeof *paths);
            const char **props_shas = calloc((size_t)np, sizeof *props_shas);
            char       **props_sha_copies = calloc((size_t)np, sizeof *props_sha_copies);
            int i = 0;
            for (cJSON *p = jprops->child; p; p = p->next) {
                paths[i] = p->string ? p->string : "";
                /* p is an object; its children are key:value string pairs. */
                int nk = 0;
                for (cJSON *kv = p->child; kv; kv = kv->next) nk++;
                const char **keys   = calloc((size_t)(nk > 0 ? nk : 1), sizeof *keys);
                const char **values = calloc((size_t)(nk > 0 ? nk : 1), sizeof *values);
                int k = 0;
                for (cJSON *kv = p->child; kv; kv = kv->next) {
                    if (cJSON_IsString(kv)) {
                        keys[k]   = kv->string ? kv->string : "";
                        values[k] = kv->valuestring ? kv->valuestring : "";
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
        int np = 0;
        for (cJSON *it = jacl->child; it; it = it->next) np++;
        if (np > 0) {
            const char **paths     = calloc((size_t)np, sizeof *paths);
            const char **acl_shas  = calloc((size_t)np, sizeof *acl_shas);
            char       **sha_copies = calloc((size_t)np, sizeof *sha_copies);
            int i = 0;
            for (cJSON *p = jacl->child; p; p = p->next) {
                if (!cJSON_IsArray(p)) continue;
                int nr = cJSON_GetArraySize(p);
                if (nr == 0) continue;   /* empty = remove */
                const char **rules = calloc((size_t)nr, sizeof *rules);
                int rcount = 0;
                for (int k = 0; k < nr; k++) {
                    cJSON *r = cJSON_GetArrayItem(p, k);
                    if (cJSON_IsString(r)) rules[rcount++] = r->valuestring;
                }
                const char *asha = svnae_build_acl_blob(repo, rules, rcount);
                free(rules);
                if (!asha) asha = "";
                paths[i] = p->string ? p->string : "";
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
    if (new_rev < 0) {
        respond_error(res, 500, "commit failed");
        return;
    }

    char buf[64];
    snprintf(buf, sizeof buf, "{\"rev\":%d}", new_rev);
    respond_json(res, 200, buf);
}

/* Recursively walk `path` at `rev` and confirm that `user` has RW on
 * every descendant (including `path` itself). Returns 1 if fully RW,
 * 0 if any descendant denies RW to the caller. Used by the /copy
 * handler to enforce "you can only copy what you fully own". */
static int
acl_user_has_rw_subtree(const char *repo, int rev, const char *user,
                        const char *path)
{
    if (!acl_allows_mode(repo, rev, user, path, 0)) return 0;
    if (!acl_allows_mode(repo, rev, user, path, 1)) return 0;

    /* If it's a dir, descend. Files have no descendants. */
    char sha[65] = {0};
    char kind = 0;
    if (!svnae_repos_resolve(repo, rev, path, sha, &kind)) return 1;
    if (kind != 'd') return 1;

    struct svnae_list *L = svnae_repos_list(repo, rev, *path ? path : "/");
    if (!L) return 1;
    int n = svnae_repos_list_count(L);
    int ok = 1;
    for (int i = 0; i < n; i++) {
        const char *name = svnae_repos_list_name(L, i);
        char child[PATH_MAX];
        if (*path) snprintf(child, sizeof child, "%s/%s", path, name);
        else       snprintf(child, sizeof child, "%s", name);
        if (!acl_user_has_rw_subtree(repo, rev, user, child)) { ok = 0; break; }
    }
    svnae_repos_list_free(L);
    return ok;
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

static char *
auto_follow_copy_acl(const char *repo, int base_rev,
                    const char *from_path, const char *to_path)
{
    /* Read the base rev's paths-acl blob. */
    char *acl_root = load_rev_blob_field(repo, base_rev, "acl");
    if (!acl_root || !*acl_root) { free(acl_root); return NULL; }
    char *body = svnae_rep_read_blob(repo, acl_root);
    free(acl_root);
    if (!body) return NULL;

    /* First pass: count matches AND collect all existing entries so
     * the new paths-acl blob preserves every original plus the new
     * rebased entries. */
    int cap = 16, n = 0;
    char **paths    = calloc((size_t)cap, sizeof *paths);
    char **acl_shas = calloc((size_t)cap, sizeof *acl_shas);

    size_t from_len = strlen(from_path);

    const char *p = body;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        const char *sp = memchr(p, ' ', llen);
        if (sp) {
            size_t sha_len = (size_t)(sp - p);
            size_t name_len = llen - sha_len - 1;
            if (sha_len < 65 && name_len < PATH_MAX) {
                char sha[65], this_path[PATH_MAX];
                memcpy(sha, p, sha_len); sha[sha_len] = '\0';
                memcpy(this_path, sp + 1, name_len); this_path[name_len] = '\0';

                /* Preserve the existing entry verbatim. */
                if (n == cap) {
                    cap *= 2;
                    paths    = realloc(paths,    (size_t)cap * sizeof *paths);
                    acl_shas = realloc(acl_shas, (size_t)cap * sizeof *acl_shas);
                }
                paths[n]    = strdup(this_path);
                acl_shas[n] = strdup(sha);
                n++;

                /* Does this entry live under from_path? If so emit a
                 * parallel entry at to_path/<tail>. Exact-match
                 * becomes to_path itself. */
                int rebase = 0;
                if (strcmp(this_path, from_path) == 0) {
                    rebase = 1;
                } else if (name_len > from_len
                           && memcmp(this_path, from_path, from_len) == 0
                           && this_path[from_len] == '/') {
                    rebase = 1;
                }
                if (rebase) {
                    char rebased[PATH_MAX];
                    if (strcmp(this_path, from_path) == 0) {
                        snprintf(rebased, sizeof rebased, "%s", to_path);
                    } else {
                        /* tail = this_path[from_len+1..] */
                        snprintf(rebased, sizeof rebased, "%s/%s",
                                 to_path, this_path + from_len + 1);
                    }
                    if (n == cap) {
                        cap *= 2;
                        paths    = realloc(paths,    (size_t)cap * sizeof *paths);
                        acl_shas = realloc(acl_shas, (size_t)cap * sizeof *acl_shas);
                    }
                    paths[n]    = strdup(rebased);
                    acl_shas[n] = strdup(sha);
                    n++;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    svnae_rep_free(body);

    if (n == 0) {
        free(paths); free(acl_shas);
        return NULL;
    }

    const char **cpaths    = calloc((size_t)n, sizeof *cpaths);
    const char **cacl_shas = calloc((size_t)n, sizeof *cacl_shas);
    for (int i = 0; i < n; i++) {
        cpaths[i] = paths[i];
        cacl_shas[i] = acl_shas[i];
    }
    const char *new_blob = svnae_build_paths_acl_blob(repo, cpaths, cacl_shas, n);
    char *out = new_blob ? strdup(new_blob) : NULL;

    for (int i = 0; i < n; i++) { free(paths[i]); free(acl_shas[i]); }
    free(paths); free(acl_shas);
    free(cpaths); free(cacl_shas);
    return out;
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
static void
handle_repo_copy(HttpRequest *req, HttpServerResponse *res, void *user_data)
{
    (void)user_data;
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail || strcmp(tail, "/copy") != 0) {
        respond_error(res, 404, "not found");
        return;
    }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }
    if (!req->body || req->body_length == 0) {
        respond_error(res, 400, "empty body");
        return;
    }
    cJSON *root = cJSON_ParseWithLength(req->body, req->body_length);
    if (!root) { respond_error(res, 400, "malformed JSON"); return; }

    cJSON *jbase = cJSON_GetObjectItemCaseSensitive(root, "base_rev");
    cJSON *jfrom = cJSON_GetObjectItemCaseSensitive(root, "from_path");
    cJSON *jto   = cJSON_GetObjectItemCaseSensitive(root, "to_path");
    cJSON *jaut  = cJSON_GetObjectItemCaseSensitive(root, "author");
    cJSON *jlog  = cJSON_GetObjectItemCaseSensitive(root, "log");
    if (!cJSON_IsNumber(jbase) || !cJSON_IsString(jfrom) ||
        !cJSON_IsString(jto)   || !cJSON_IsString(jaut)  ||
        !cJSON_IsString(jlog)) {
        cJSON_Delete(root);
        respond_error(res, 400, "missing or wrong-typed field");
        return;
    }

    int base_rev = jbase->valueint;
    char *from_path = strdup(jfrom->valuestring);
    char *to_path   = strdup(jto->valuestring);
    char *author    = strdup(jaut->valuestring);
    char *logmsg    = strdup(jlog->valuestring);
    cJSON_Delete(root);

    /* ACL check (Phase 7.7):
     *   You can only server-copy what you already have RW on in
     *   full. Walk from_path's subtree and require RW everywhere.
     *   Caller also needs write on to_path. Super-user bypasses.
     *
     *   If the check passes, the copy proceeds and ACLs auto-follow:
     *   every paths-acl entry under from_path gets a parallel entry
     *   rebased under to_path, so the branch inherits its source's
     *   restrictions verbatim. Nothing's visibility changes from
     *   any other user's perspective.
     */
    {
        const char *user = NULL;
        int is_super = auth_context(req, &user);
        if (!is_super) {
            if (!acl_user_has_rw_subtree(repo, base_rev, user, from_path)) {
                free(from_path); free(to_path); free(author); free(logmsg);
                respond_error(res, 403, "forbidden");
                return;
            }
            if (!acl_allows_mode(repo, base_rev, user, to_path, 1)) {
                free(from_path); free(to_path); free(author); free(logmsg);
                respond_error(res, 403, "forbidden");
                return;
            }
        }
        /* Phase 8.2b: server-side cp must stay inside one branch. Both
         * endpoints have to satisfy the branch's include spec. Cross-
         * branch cp is refused — the user should use `svn branch create`
         * to seed one branch from another. */
        const char *cp_branch = request_branch(req);
        if (!spec_allows(repo, cp_branch, from_path, is_super) ||
            !spec_allows(repo, cp_branch, to_path,   is_super)) {
            free(from_path); free(to_path); free(author); free(logmsg);
            respond_error(res, 403, "cross-branch copy refused");
            return;
        }
    }

    char sha1[65];
    char kind_char;
    if (!svnae_repos_resolve(repo, base_rev, from_path, sha1, &kind_char)) {
        free(from_path); free(to_path); free(author); free(logmsg);
        respond_error(res, 404, "from_path not found at base_rev");
        return;
    }

    /* Compute the new rev's paths-acl blob sha (if any ACLs follow). */
    char *new_acl_sha = auto_follow_copy_acl(repo, base_rev, from_path, to_path);

    struct svnae_txn *txn = svnae_txn_new(base_rev);
    svnae_txn_copy(txn, to_path, sha1, kind_char == 'd' ? 1 : 0);
    int new_rev = svnae_commit_finalise_with_acl(repo, txn, author, logmsg,
                                                  "", /* props inherit */
                                                  new_acl_sha ? new_acl_sha : "");
    svnae_txn_free(txn);
    free(from_path); free(to_path); free(author); free(logmsg);
    free(new_acl_sha);

    if (new_rev < 0) { respond_error(res, 500, "copy commit failed"); return; }
    char buf[64];
    snprintf(buf, sizeof buf, "{\"rev\":%d}", new_rev);
    respond_json(res, 200, buf);
}

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

static void
handle_repo_branch_create(HttpRequest *req, HttpServerResponse *res,
                          const char *branch_name)
{
    char name[128];
    const char *tail = parse_repo_and_tail(req->path, name, sizeof name);
    if (!tail) { respond_error(res, 404, "not found"); return; }
    const char *repo = find_repo_path(name);
    if (!repo) { respond_error(res, 404, "no such repo"); return; }
    if (!branch_name || !*branch_name) {
        respond_error(res, 400, "missing branch name"); return;
    }

    /* Super-user check. Branch creation is privileged. */
    const char *user = NULL;
    int is_super = auth_context(req, &user);
    if (!is_super) { respond_error(res, 403, "forbidden"); return; }

    if (!req->body || req->body_length == 0) {
        respond_error(res, 400, "empty body"); return;
    }
    cJSON *root = cJSON_ParseWithLength(req->body, req->body_length);
    if (!root) { respond_error(res, 400, "malformed JSON"); return; }

    cJSON *jbase = cJSON_GetObjectItemCaseSensitive(root, "base");
    cJSON *jinc  = cJSON_GetObjectItemCaseSensitive(root, "include");
    if (!cJSON_IsString(jbase) || !cJSON_IsArray(jinc)) {
        cJSON_Delete(root);
        respond_error(res, 400, "need base:string and include:array");
        return;
    }
    int n_inc = cJSON_GetArraySize(jinc);
    if (n_inc <= 0) {
        cJSON_Delete(root);
        respond_error(res, 400, "include must be non-empty");
        return;
    }

    /* Collect include globs as a flat C array. */
    const char **globs = calloc((size_t)n_inc, sizeof *globs);
    char **glob_copies = calloc((size_t)n_inc, sizeof *glob_copies);
    int gi = 0;
    for (int i = 0; i < n_inc; i++) {
        cJSON *e = cJSON_GetArrayItem(jinc, i);
        if (cJSON_IsString(e) && e->valuestring && *e->valuestring) {
            glob_copies[gi] = strdup(e->valuestring);
            globs[gi] = glob_copies[gi];
            gi++;
        }
    }
    char *base_copy = strdup(jbase->valuestring);
    cJSON_Delete(root);

    int new_rev = svnae_branch_create(repo, branch_name, base_copy,
                                      globs, gi,
                                      (user && *user) ? user : "super");

    for (int i = 0; i < gi; i++) free(glob_copies[i]);
    free(glob_copies); free(globs); free(base_copy);

    if (new_rev < 0) {
        respond_error(res, 400, "branch create failed (exists, bad base, or no globs?)");
        return;
    }
    char buf[128];
    snprintf(buf, sizeof buf, "{\"rev\":%d,\"branch\":\"%s\"}", new_rev, branch_name);
    http_response_set_status(res, 201);
    http_response_json(res, buf);
}

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
        if (strcmp(tail, "/commit") == 0) { handle_repo_commit(req, res, user_data); return; }
        if (strcmp(tail, "/copy")   == 0) { handle_repo_copy  (req, res, user_data); return; }

        /* /branches/<NAME>/create — Phase 8.2a */
        if (strncmp(tail, "/branches/", 10) == 0) {
            const char *after = tail + 10;
            const char *slash = strchr(after, '/');
            if (slash && strcmp(slash, "/create") == 0) {
                char br_name[128];
                size_t nlen = (size_t)(slash - after);
                if (nlen > 0 && nlen < sizeof br_name) {
                    memcpy(br_name, after, nlen);
                    br_name[nlen] = '\0';
                    handle_repo_branch_create(req, res, br_name);
                    return;
                }
            }
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
