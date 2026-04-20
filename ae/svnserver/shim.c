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

/* Parse a per-path ACL blob body ("+alice\n-eve\n") and decide
 * whether `user` is allowed. Returns 1 allow, 0 deny, -1 no-match. */
static int
acl_body_decide(const char *body, const char *user)
{
    if (!body) return -1;
    int wild_allow = -1, wild_deny = -1;
    int user_allow = -1, user_deny = -1;
    const char *p = body;
    while (*p) {
        char sign = *p;
        if (sign != '+' && sign != '-') {
            const char *eol = strchr(p, '\n');
            p = eol ? eol + 1 : p + strlen(p);
            continue;
        }
        p++;
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char name[128];
        if (n < sizeof name) {
            memcpy(name, p, n); name[n] = '\0';
            if (strcmp(name, "*") == 0) {
                if (sign == '+') wild_allow = 1;
                else             wild_deny  = 1;
            } else if (strcmp(name, user) == 0) {
                if (sign == '+') user_allow = 1;
                else             user_deny  = 1;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    /* Priority: explicit user rule > wildcard. Deny wins ties. */
    if (user_deny  == 1) return 0;
    if (user_allow == 1) return 1;
    if (wild_deny  == 1) return 0;
    if (wild_allow == 1) return 1;
    return -1;
}

/* Scan the paths-acl blob body for a path entry. Returns a malloc'd
 * 64+NUL sha of the matching per-path ACL blob, or NULL if absent. */
static char *
paths_acl_lookup(const char *body, const char *path)
{
    if (!body) return NULL;
    size_t plen = strlen(path);
    const char *p = body;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        /* Format: "<sha-hex> <path>". Sha width is repo-algo-dependent. */
        const char *sp = memchr(p, ' ', llen);
        if (sp) {
            size_t sha_len = (size_t)(sp - p);
            size_t name_len = llen - sha_len - 1;
            if (name_len == plen && memcmp(sp + 1, path, plen) == 0 && sha_len < 65) {
                char *out = malloc(sha_len + 1);
                memcpy(out, p, sha_len);
                out[sha_len] = '\0';
                return out;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return NULL;
}

/* Decide whether `user` can see `target_path` at `rev`. Walks path
 * upward through the paths-acl blob, nearest match wins. Returns
 * 1 allow / 0 deny. Empty path = root. Open by default if no rule
 * applies anywhere in the ancestry. */
static int
acl_allows(const char *repo, int rev, const char *user, const char *target_path)
{
    /* Superuser (anywhere above this fn) already short-circuited the
     * caller; we never see them here. */
    char *acl_root = load_rev_blob_field(repo, rev, "acl");
    if (!acl_root || !*acl_root) { free(acl_root); return 1; }
    char *paths_body = svnae_rep_read_blob(repo, acl_root);
    free(acl_root);
    if (!paths_body) return 1;  /* corrupt or missing — default open */

    /* Walk from the full path up to "". */
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
                int d = acl_body_decide(rules, user);
                svnae_rep_free(rules);
                if (d == 0) { svnae_rep_free(paths_body); return 0; }
                if (d == 1) { svnae_rep_free(paths_body); return 1; }
            }
            /* No-match in this ACL — keep walking up. */
        }
        if (!*buf) break;
        char *last = strrchr(buf, '/');
        if (last) *last = '\0';
        else      buf[0] = '\0';
    }
    svnae_rep_free(paths_body);
    return 1;
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
    const char *p = body;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (llen >= 4) {
            char kind_c = p[0];
            const char *sha_start = p + 2;
            const char *sp2 = memchr(sha_start, ' ', llen - 2);
            if (sp2) {
                size_t sha_len = (size_t)(sp2 - sha_start);
                size_t name_off = 2 + sha_len + 1;
                size_t name_len = llen - name_off;
                char child_sha[65], child_name[PATH_MAX];
                if (sha_len < sizeof child_sha && name_len < sizeof child_name) {
                    memcpy(child_sha, sha_start, sha_len); child_sha[sha_len] = '\0';
                    memcpy(child_name, p + name_off, name_len); child_name[name_len] = '\0';
                    char child_path[PATH_MAX];
                    if (*prefix) snprintf(child_path, sizeof child_path, "%s/%s",
                                          prefix, child_name);
                    else         snprintf(child_path, sizeof child_path, "%s",
                                          child_name);
                    int allowed = acl_allows(repo, rev, user, child_path);
                    if (allowed) {
                        /* Descend: the effective hash of a subdir may
                         * itself be redacted if grandchildren are
                         * denied. Files pass through as-is. */
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
            }
        }
        if (!eol) break;
        p = eol + 1;
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

static void
sb_putjson_string(struct sb *s, const char *v)
{
    sb_putc(s, '"');
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        unsigned char c = *p;
        switch (c) {
            case '"':  sb_puts(s, "\\\""); break;
            case '\\': sb_puts(s, "\\\\"); break;
            case '\n': sb_puts(s, "\\n");  break;
            case '\r': sb_puts(s, "\\r");  break;
            case '\t': sb_puts(s, "\\t");  break;
            case '\b': sb_puts(s, "\\b");  break;
            case '\f': sb_puts(s, "\\f");  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    sb_puts(s, buf);
                } else {
                    sb_putc(s, (char)c);
                }
        }
    }
    sb_putc(s, '"');
}

static void
sb_putjson_int(struct sb *s, int v)
{
    char buf[32];
    snprintf(buf, sizeof buf, "%d", v);
    sb_puts(s, buf);
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
    struct sb s = {0};
    sb_puts(&s, "{\"head\":");
    sb_putjson_int(&s, head);
    sb_puts(&s, ",\"name\":");
    sb_putjson_string(&s, name);
    sb_puts(&s, ",\"hash_algo\":");
    sb_putjson_string(&s, svnae_repo_primary_hash(repo));
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
        sb_puts(&s, "{\"rev\":");
        sb_putjson_int(&s, rev);
        sb_puts(&s, ",\"author\":");
        sb_putjson_string(&s, svnae_repos_log_author(lg, i));
        sb_puts(&s, ",\"date\":");
        sb_putjson_string(&s, svnae_repos_log_date(lg, i));
        sb_puts(&s, ",\"msg\":");
        sb_putjson_string(&s, svnae_repos_log_msg(lg, i));
        sb_putc(&s, '}');
    }
    sb_puts(&s, "]}");
    svnae_repos_log_free(lg);
    respond_json(res, 200, s.data ? s.data : "{\"entries\":[]}");
    free(s.data);
}

/* Load a "key: value" line from the rev blob of rev `rev`. Returns a
 * malloc'd value (caller frees with free()), or NULL if key absent. */
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
    char needle[64];
    snprintf(needle, sizeof needle, "%s: ", key);
    char *p = strstr(body, needle);
    char *out = NULL;
    if (p) {
        p += strlen(needle);
        char *eol = strchr(p, '\n');
        size_t L = eol ? (size_t)(eol - p) : strlen(p);
        out = malloc(L + 1);
        memcpy(out, p, L);
        out[L] = '\0';
    }
    svnae_rep_free(body);
    return out;
}

/* Back-compat: one call site still asks for "props:" specifically. */
static char *
load_rev_props_sha1(const char *repo, int rev)
{
    return load_rev_blob_field(repo, rev, "props");
}

/* Given a paths-props blob body and a target path, return the sha1 of
 * that path's per-path props blob (malloc'd), or NULL if missing. */
static char *
paths_props_lookup(const char *body, const char *path)
{
    if (!body) return NULL;
    size_t plen = strlen(path);
    const char *p = body;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        /* format: "<40-char sha> <path>" */
        if (llen > 41) {
            size_t name_len = llen - 41;
            if (name_len == plen && memcmp(p + 41, path, plen) == 0) {
                char *out = malloc(41);
                memcpy(out, p, 40);
                out[40] = '\0';
                return out;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return NULL;
}

/* Given a per-path props blob (key=value\n lines), emit it as JSON
 * {"k":"v",...} into sb. */
static void
sb_put_props_as_json(struct sb *s, const char *body)
{
    sb_putc(s, '{');
    int first = 1;
    const char *p = body;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        const char *eq = memchr(p, '=', llen);
        if (eq) {
            size_t klen = (size_t)(eq - p);
            size_t vlen = llen - klen - 1;
            if (!first) sb_putc(s, ',');
            first = 0;
            char kbuf[256];
            if (klen < sizeof kbuf) {
                memcpy(kbuf, p, klen); kbuf[klen] = '\0';
                sb_putjson_string(s, kbuf);
            } else {
                sb_puts(s, "\"\"");
            }
            sb_putc(s, ':');
            char *vbuf = malloc(vlen + 1);
            memcpy(vbuf, eq + 1, vlen); vbuf[vlen] = '\0';
            sb_putjson_string(s, vbuf);
            free(vbuf);
        }
        if (!eol) break;
        p = eol + 1;
    }
    sb_putc(s, '}');
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
        struct sb s = {0};
        sb_puts(&s, "{\"rev\":");
        sb_putjson_int(&s, svnae_repos_info_rev_num(I));
        sb_puts(&s, ",\"author\":");
        sb_putjson_string(&s, svnae_repos_info_author(I));
        sb_puts(&s, ",\"date\":");
        sb_putjson_string(&s, svnae_repos_info_date(I));
        sb_puts(&s, ",\"msg\":");
        sb_putjson_string(&s, svnae_repos_info_msg(I));
        sb_puts(&s, ",\"root\":");
        sb_putjson_string(&s, reported_root);
        sb_puts(&s, "}");
        free(root_sha);
        svnae_repos_info_free(I);
        respond_json(res, 200, s.data ? s.data : "{}");
        free(s.data);
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
            sb_puts(&s, "{\"action\":");
            sb_putjson_string(&s, svnae_repos_paths_action(P, i));
            sb_puts(&s, ",\"path\":");
            sb_putjson_string(&s, path);
            sb_putc(&s, '}');
        }
        sb_puts(&s, "]}");
        svnae_repos_paths_free(P);
        respond_json(res, 200, s.data ? s.data : "{\"entries\":[]}");
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
        const char *p = dir_body;
        const char *algo = svnae_repo_primary_hash(repo);
        while (*p) {
            const char *eol = strchr(p, '\n');
            size_t llen = eol ? (size_t)(eol - p) : strlen(p);
            if (llen >= 4) {
                char kind_c = p[0];
                const char *sha_start = p + 2;
                const char *sp2 = memchr(sha_start, ' ', llen - 2);
                if (sp2) {
                    size_t sha_len = (size_t)(sp2 - sha_start);
                    size_t name_off = 2 + sha_len + 1;
                    size_t name_len = llen - name_off;
                    char child_sha[65], child_name[PATH_MAX];
                    if (sha_len < sizeof child_sha && name_len < sizeof child_name) {
                        memcpy(child_sha, sha_start, sha_len); child_sha[sha_len] = '\0';
                        memcpy(child_name, p + name_off, name_len); child_name[name_len] = '\0';

                        /* Build the child's full repo-relative path. */
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

                            /* Preserve the raw line verbatim in the
                             * redact buffer. */
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
                }
            }
            if (!eol) break;
            p = eol + 1;
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

    char sha1[65];
    char kind_char;
    if (!svnae_repos_resolve(repo, base_rev, from_path, sha1, &kind_char)) {
        free(from_path); free(to_path); free(author); free(logmsg);
        respond_error(res, 404, "from_path not found at base_rev");
        return;
    }

    struct svnae_txn *txn = svnae_txn_new(base_rev);
    svnae_txn_copy(txn, to_path, sha1, kind_char == 'd' ? 1 : 0);
    int new_rev = svnae_commit_finalise(repo, txn, author, logmsg);
    svnae_txn_free(txn);
    free(from_path); free(to_path); free(author); free(logmsg);

    if (new_rev < 0) { respond_error(res, 500, "copy commit failed"); return; }
    char buf[64];
    snprintf(buf, sizeof buf, "{\"rev\":%d}", new_rev);
    respond_json(res, 200, buf);
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
        respond_error(res, 404, "unknown POST route");
        return;
    }

    if (strcmp(tail, "/info") == 0)      { handle_repo_info(req, res, user_data); return; }
    if (strcmp(tail, "/log") == 0)       { handle_repo_log (req, res, user_data); return; }
    if (strncmp(tail, "/rev/", 5) == 0)  { handle_repo_rev (req, res, user_data); return; }

    respond_error(res, 404, "unknown sub-route");
}

void *svnae_svnserver_handler_dispatch(void) { return (void *)dispatch; }
