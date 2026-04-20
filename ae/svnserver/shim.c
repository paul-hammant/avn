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
void              svnae_rep_free(char *p);

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

/* --- JSON string builder ---------------------------------------------- */

struct sb { char *data; size_t len, cap; };

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

    struct sb s = {0};
    sb_puts(&s, "{\"entries\":[");
    int n = svnae_repos_log_count(lg);
    for (int i = 0; i < n; i++) {
        if (i) sb_putc(&s, ',');
        sb_puts(&s, "{\"rev\":");
        sb_putjson_int(&s, svnae_repos_log_rev(lg, i));
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

/* GET /repos/{r}/rev/:rev/info  → info_rev JSON
 * GET /repos/{r}/rev/:rev/cat/<path>   → raw bytes
 * GET /repos/{r}/rev/:rev/list/<path>  → list JSON
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
        struct sb s = {0};
        sb_puts(&s, "{\"rev\":");
        sb_putjson_int(&s, svnae_repos_info_rev_num(I));
        sb_puts(&s, ",\"author\":");
        sb_putjson_string(&s, svnae_repos_info_author(I));
        sb_puts(&s, ",\"date\":");
        sb_putjson_string(&s, svnae_repos_info_date(I));
        sb_puts(&s, ",\"msg\":");
        sb_putjson_string(&s, svnae_repos_info_msg(I));
        sb_puts(&s, "}");
        svnae_repos_info_free(I);
        respond_json(res, 200, s.data ? s.data : "{}");
        free(s.data);
        return;
    }

    /* /rev/N/cat/<path>   (path may be empty → root, which is an error for cat) */
    if (strncmp(after, "/cat/", 5) == 0 || strcmp(after, "/cat") == 0) {
        const char *file_path = strcmp(after, "/cat") == 0 ? "" : after + 5;
        if (!*file_path) { respond_error(res, 400, "cat requires a path"); return; }
        char *data = svnae_repos_cat(repo, rev, file_path);
        if (!data) { respond_error(res, 404, "not found"); return; }
        /* We don't know the length without a separate API call. Our cat
         * returns NUL-terminated text for the test cases; for this Phase 6
         * we use strlen. When binary blobs become common we'll add a
         * length-aware cat variant. */
        size_t len = strlen(data);
        respond_binary(res, data, len, "application/octet-stream");
        svnae_rep_free(data);
        return;
    }

    /* /rev/N/list/<path>  (path "" → root) */
    if (strncmp(after, "/list/", 6) == 0 || strcmp(after, "/list") == 0) {
        const char *dir_path = strcmp(after, "/list") == 0 ? "" : after + 6;
        struct svnae_list *L = svnae_repos_list(repo, rev, *dir_path ? dir_path : "/");
        if (!L) { respond_error(res, 404, "not a directory"); return; }
        struct sb s = {0};
        sb_puts(&s, "{\"entries\":[");
        int n = svnae_repos_list_count(L);
        for (int i = 0; i < n; i++) {
            if (i) sb_putc(&s, ',');
            sb_puts(&s, "{\"name\":");
            sb_putjson_string(&s, svnae_repos_list_name(L, i));
            sb_puts(&s, ",\"kind\":");
            sb_putjson_string(&s, svnae_repos_list_kind(L, i));
            sb_putc(&s, '}');
        }
        sb_puts(&s, "]}");
        svnae_repos_list_free(L);
        respond_json(res, 200, s.data ? s.data : "{\"entries\":[]}");
        free(s.data);
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
    cJSON_Delete(root);

    int new_rev = svnae_commit_finalise(repo, txn, author, logmsg);
    svnae_txn_free(txn);
    free(author);
    free(logmsg);
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

    char sha1[41];
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
