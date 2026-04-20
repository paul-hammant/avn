/* ra/shim.c — libsvn_ra: REST client to aether-svnserver.
 *
 * Mirrors repos/shim.c's query shape, but over HTTP + JSON. The handles
 * returned here (svnae_ra_log, svnae_ra_list, svnae_ra_info) look and
 * behave the same as their server-side counterparts so Aether callers
 * that already know the repos query API can switch repository sources
 * without reshape.
 *
 * We do the HTTP and JSON work ourselves rather than going through
 * std.http and cjson via Aether FFI, because Aether can't model the
 * function-pointer and opaque-ptr chains cleanly. std.http's `get`
 * wrapper is Aether-only; we drop to libcurl for the underlying
 * request/response.
 *
 * Dependency: libcurl. Installed via `apt install libcurl4-openssl-dev`.
 */

#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- HTTP plumbing ---------------------------------------------------- */

struct buf { char *data; size_t len; size_t cap; };

static size_t
buf_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    struct buf *b = userp;
    size_t incoming = size * nmemb;
    if (b->len + incoming + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + incoming + 1) nc *= 2;
        b->data = realloc(b->data, nc);
        b->cap = nc;
    }
    memcpy(b->data + b->len, contents, incoming);
    b->len += incoming;
    b->data[b->len] = '\0';
    return incoming;
}

/* Perform GET. Returns (body, status). On success body is malloc'd NUL-
 * terminated; caller frees. On CURL-level failure body is NULL. */
static int
http_get(const char *url, char **out_body, size_t *out_len, int *out_status)
{
    CURL *h = curl_easy_init();
    if (!h) return -1;
    struct buf b = {0};
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, buf_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) { free(b.data); return -1; }

    *out_body = b.data;
    *out_len = b.len;
    *out_status = (int)status;
    return 0;
}

static int
http_post_json(const char *url, const char *body, char **out_resp, size_t *out_len, int *out_status)
{
    CURL *h = curl_easy_init();
    if (!h) return -1;
    struct buf b = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, buf_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) { free(b.data); return -1; }

    *out_resp = b.data;
    *out_len = b.len;
    *out_status = (int)status;
    return 0;
}

/* ---- public API ------------------------------------------------------ */

/* head_rev: returns the current revision number, or -1 on any failure. */
int
svnae_ra_head_rev(const char *base_url, const char *repo_name)
{
    char url[1024];
    snprintf(url, sizeof url, "%s/repos/%s/info", base_url, repo_name);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return -1;
    if (status != 200) { free(body); return -1; }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return -1;
    cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "head");
    int rev = cJSON_IsNumber(h) ? h->valueint : -1;
    cJSON_Delete(root);
    return rev;
}

/* ---- log handle (matches repos/shim.c accessor shape) ---------------- */

struct log_entry { int rev; char *author; char *date; char *msg; };
struct svnae_ra_log { struct log_entry *entries; int n; };

struct svnae_ra_log *
svnae_ra_log(const char *base_url, const char *repo_name)
{
    char url[1024];
    snprintf(url, sizeof url, "%s/repos/%s/log", base_url, repo_name);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return NULL;
    cJSON *jentries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(jentries)) { cJSON_Delete(root); return NULL; }

    int n = cJSON_GetArraySize(jentries);
    struct svnae_ra_log *lg = calloc(1, sizeof *lg);
    lg->n = n;
    lg->entries = calloc((size_t)n, sizeof *lg->entries);

    cJSON *e;
    int i = 0;
    cJSON_ArrayForEach(e, jentries) {
        cJSON *jr = cJSON_GetObjectItemCaseSensitive(e, "rev");
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(e, "author");
        cJSON *jd = cJSON_GetObjectItemCaseSensitive(e, "date");
        cJSON *jm = cJSON_GetObjectItemCaseSensitive(e, "msg");
        lg->entries[i].rev    = cJSON_IsNumber(jr) ? jr->valueint : -1;
        lg->entries[i].author = cJSON_IsString(ja) ? strdup(ja->valuestring) : strdup("");
        lg->entries[i].date   = cJSON_IsString(jd) ? strdup(jd->valuestring) : strdup("");
        lg->entries[i].msg    = cJSON_IsString(jm) ? strdup(jm->valuestring) : strdup("");
        i++;
    }
    cJSON_Delete(root);
    return lg;
}

int svnae_ra_log_count(const struct svnae_ra_log *lg) { return lg ? lg->n : 0; }

int
svnae_ra_log_rev(const struct svnae_ra_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n) return -1;
    return lg->entries[i].rev;
}

const char *
svnae_ra_log_author(const struct svnae_ra_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n) return "";
    return lg->entries[i].author;
}

const char *
svnae_ra_log_date(const struct svnae_ra_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n) return "";
    return lg->entries[i].date;
}

const char *
svnae_ra_log_msg(const struct svnae_ra_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n) return "";
    return lg->entries[i].msg;
}

void
svnae_ra_log_free(struct svnae_ra_log *lg)
{
    if (!lg) return;
    for (int i = 0; i < lg->n; i++) {
        free(lg->entries[i].author);
        free(lg->entries[i].date);
        free(lg->entries[i].msg);
    }
    free(lg->entries);
    free(lg);
}

/* ---- info handle ----------------------------------------------------- */

struct svnae_ra_info { int rev; char *author; char *date; char *msg; };

struct svnae_ra_info *
svnae_ra_info_rev(const char *base_url, const char *repo_name, int rev)
{
    char url[1024];
    snprintf(url, sizeof url, "%s/repos/%s/rev/%d/info", base_url, repo_name, rev);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return NULL;
    struct svnae_ra_info *I = calloc(1, sizeof *I);
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "rev");
    cJSON *ja = cJSON_GetObjectItemCaseSensitive(root, "author");
    cJSON *jd = cJSON_GetObjectItemCaseSensitive(root, "date");
    cJSON *jm = cJSON_GetObjectItemCaseSensitive(root, "msg");
    I->rev    = cJSON_IsNumber(jr) ? jr->valueint : -1;
    I->author = cJSON_IsString(ja) ? strdup(ja->valuestring) : strdup("");
    I->date   = cJSON_IsString(jd) ? strdup(jd->valuestring) : strdup("");
    I->msg    = cJSON_IsString(jm) ? strdup(jm->valuestring) : strdup("");
    cJSON_Delete(root);
    return I;
}

int         svnae_ra_info_rev_num(const struct svnae_ra_info *I) { return I ? I->rev : -1; }
const char *svnae_ra_info_author (const struct svnae_ra_info *I) { return I ? I->author : ""; }
const char *svnae_ra_info_date   (const struct svnae_ra_info *I) { return I ? I->date : ""; }
const char *svnae_ra_info_msg    (const struct svnae_ra_info *I) { return I ? I->msg : ""; }

void
svnae_ra_info_free(struct svnae_ra_info *I)
{
    if (!I) return;
    free(I->author); free(I->date); free(I->msg);
    free(I);
}

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
    char url[2048];
    snprintf(url, sizeof url, "%s/repos/%s/rev/%d/cat/%s", base_url, repo_name, rev, path);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }
    return body;  /* caller owns */
}

void svnae_ra_free(char *p) { free(p); }

/* --- remote properties ---------------------------------------------- *
 *
 * GET /repos/{r}/rev/{n}/props/<path> returns a JSON {k:v,...} object.
 * We expose it as a handle with indexed name/value accessors.
 */

struct prop_entry_ra { char *name; char *value; };
struct svnae_ra_props { struct prop_entry_ra *items; int n; };

struct svnae_ra_props *
svnae_ra_get_props(const char *base_url, const char *repo_name,
                   int rev, const char *path)
{
    while (*path == '/') path++;
    char url[2048];
    if (*path) {
        snprintf(url, sizeof url, "%s/repos/%s/rev/%d/props/%s",
                 base_url, repo_name, rev, path);
    } else {
        snprintf(url, sizeof url, "%s/repos/%s/rev/%d/props",
                 base_url, repo_name, rev);
    }
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return NULL;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }

    int n = 0;
    for (cJSON *it = root->child; it; it = it->next) n++;
    struct svnae_ra_props *P = calloc(1, sizeof *P);
    P->n = n;
    P->items = calloc((size_t)(n > 0 ? n : 1), sizeof *P->items);
    int i = 0;
    for (cJSON *it = root->child; it; it = it->next) {
        P->items[i].name  = strdup(it->string ? it->string : "");
        P->items[i].value = cJSON_IsString(it) ? strdup(it->valuestring) : strdup("");
        i++;
    }
    cJSON_Delete(root);
    return P;
}

int         svnae_ra_props_count(const struct svnae_ra_props *P) { return P ? P->n : 0; }
const char *svnae_ra_props_name (const struct svnae_ra_props *P, int i) { return (P && i >= 0 && i < P->n) ? P->items[i].name  : ""; }
const char *svnae_ra_props_value(const struct svnae_ra_props *P, int i) { return (P && i >= 0 && i < P->n) ? P->items[i].value : ""; }

void
svnae_ra_props_free(struct svnae_ra_props *P)
{
    if (!P) return;
    for (int i = 0; i < P->n; i++) { free(P->items[i].name); free(P->items[i].value); }
    free(P->items);
    free(P);
}

/* --- server-side copy ------------------------------------------------ *
 *
 * POST /repos/{r}/copy with { base_rev, from_path, to_path, author, log }.
 * Returns the new revision number, or -1.
 */
int
svnae_ra_server_copy(const char *base_url, const char *repo_name,
                     int base_rev,
                     const char *from_path, const char *to_path,
                     const char *author, const char *logmsg)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "base_rev",  base_rev);
    cJSON_AddStringToObject(body, "from_path", from_path);
    cJSON_AddStringToObject(body, "to_path",   to_path);
    cJSON_AddStringToObject(body, "author",    author);
    cJSON_AddStringToObject(body, "log",       logmsg);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char url[1024];
    snprintf(url, sizeof url, "%s/repos/%s/copy", base_url, repo_name);
    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, json, &resp, &len, &status);
    free(json);

    int new_rev = -1;
    if (rc == 0 && status == 200 && resp) {
        cJSON *root = cJSON_ParseWithLength(resp, len);
        if (root) {
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "rev");
            if (cJSON_IsNumber(jr)) new_rev = jr->valueint;
            cJSON_Delete(root);
        }
    }
    free(resp);
    return new_rev;
}

/* ---- list ------------------------------------------------------------ */

struct list_entry { char *name; char kind; };
struct svnae_ra_list { struct list_entry *items; int n; };

struct svnae_ra_list *
svnae_ra_list(const char *base_url, const char *repo_name, int rev, const char *path)
{
    while (*path == '/') path++;
    char url[2048];
    if (*path) {
        snprintf(url, sizeof url, "%s/repos/%s/rev/%d/list/%s", base_url, repo_name, rev, path);
    } else {
        snprintf(url, sizeof url, "%s/repos/%s/rev/%d/list", base_url, repo_name, rev);
    }
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return NULL;
    cJSON *jentries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(jentries)) { cJSON_Delete(root); return NULL; }

    int n = cJSON_GetArraySize(jentries);
    struct svnae_ra_list *L = calloc(1, sizeof *L);
    L->n = n;
    L->items = calloc((size_t)n, sizeof *L->items);
    cJSON *e;
    int i = 0;
    cJSON_ArrayForEach(e, jentries) {
        cJSON *jn = cJSON_GetObjectItemCaseSensitive(e, "name");
        cJSON *jk = cJSON_GetObjectItemCaseSensitive(e, "kind");
        L->items[i].name = cJSON_IsString(jn) ? strdup(jn->valuestring) : strdup("");
        L->items[i].kind = (cJSON_IsString(jk) && strcmp(jk->valuestring, "dir") == 0) ? 'd' : 'f';
        i++;
    }
    cJSON_Delete(root);
    return L;
}

int svnae_ra_list_count(const struct svnae_ra_list *L) { return L ? L->n : 0; }

const char *
svnae_ra_list_name(const struct svnae_ra_list *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].name;
}

const char *
svnae_ra_list_kind(const struct svnae_ra_list *L, int i)
{
    static const char f[] = "file";
    static const char d[] = "dir";
    static const char u[] = "";
    if (!L || i < 0 || i >= L->n) return u;
    return L->items[i].kind == 'd' ? d : f;
}

void
svnae_ra_list_free(struct svnae_ra_list *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) free(L->items[i].name);
    free(L->items);
    free(L);
}

/* ---- commit ---------------------------------------------------------- *
 *
 * Aether can't build arrays across FFI easily, so we expose a builder:
 *   cb = ra_commit_begin(base_rev, author, log)
 *   ra_commit_add_file(cb, path, content, len)
 *   ra_commit_mkdir(cb, path)
 *   ra_commit_delete(cb, path)
 *   new_rev = ra_commit_finish(cb, base_url, repo_name)
 *
 * The builder accumulates edits in-memory and only serialises + POSTs
 * at finish time.
 */

struct commit_edit { int op; char *path; unsigned char *content; int content_len; };
/* op: 1=add-file, 2=mkdir, 3=delete. Matches txn_shim.c's edit kinds. */

/* Per-path props collected for commit. Parallel arrays keyed by
 * composite "path\0key" — we flatten for simplicity. When commit_finish
 * serialises, we group by path into nested objects. */
struct commit_prop { char *path; char *key; char *value; };

struct svnae_ra_commit {
    int   base_rev;
    char *author;
    char *logmsg;
    struct commit_edit *edits;
    int   n;
    int   cap;
    struct commit_prop *props;
    int   n_props;
    int   cap_props;
};

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

static int
cb_push(struct svnae_ra_commit *cb)
{
    if (cb->n == cb->cap) {
        int nc = cb->cap ? cb->cap * 2 : 8;
        struct commit_edit *p = realloc(cb->edits, (size_t)nc * sizeof *p);
        if (!p) return -1;
        cb->edits = p;
        cb->cap = nc;
    }
    return cb->n++;
}

int
svnae_ra_commit_add_file(struct svnae_ra_commit *cb, const char *path,
                         const char *content, int len)
{
    if (!cb) return -1;
    int i = cb_push(cb);
    if (i < 0) return -1;
    cb->edits[i].op = 1;
    cb->edits[i].path = strdup(path);
    if (len > 0 && content) {
        cb->edits[i].content = malloc((size_t)len);
        memcpy(cb->edits[i].content, content, (size_t)len);
        cb->edits[i].content_len = len;
    } else {
        cb->edits[i].content = NULL;
        cb->edits[i].content_len = 0;
    }
    return 0;
}

int
svnae_ra_commit_mkdir(struct svnae_ra_commit *cb, const char *path)
{
    if (!cb) return -1;
    int i = cb_push(cb);
    if (i < 0) return -1;
    cb->edits[i].op = 2;
    cb->edits[i].path = strdup(path);
    cb->edits[i].content = NULL;
    cb->edits[i].content_len = 0;
    return 0;
}

int
svnae_ra_commit_delete(struct svnae_ra_commit *cb, const char *path)
{
    if (!cb) return -1;
    int i = cb_push(cb);
    if (i < 0) return -1;
    cb->edits[i].op = 3;
    cb->edits[i].path = strdup(path);
    cb->edits[i].content = NULL;
    cb->edits[i].content_len = 0;
    return 0;
}

/* Record a property to persist on the next commit. The commit payload
 * carries ALL properties for each path the client wants to persist —
 * unmentioned paths inherit the previous revision's props. Call once
 * per (path, key, value). */
int
svnae_ra_commit_set_prop(struct svnae_ra_commit *cb,
                         const char *path, const char *key, const char *value)
{
    if (!cb) return -1;
    if (cb->n_props == cb->cap_props) {
        int nc = cb->cap_props ? cb->cap_props * 2 : 8;
        struct commit_prop *p = realloc(cb->props, (size_t)nc * sizeof *p);
        if (!p) return -1;
        cb->props = p;
        cb->cap_props = nc;
    }
    cb->props[cb->n_props].path  = strdup(path);
    cb->props[cb->n_props].key   = strdup(key);
    cb->props[cb->n_props].value = strdup(value);
    cb->n_props++;
    return 0;
}

/* base64 encode for the wire. OpenSSL EVP_EncodeBlock returns a padded
 * length; the buffer we pass must be 4*((n+2)/3)+1 bytes. */
static char *
b64_encode(const unsigned char *src, int len)
{
    int out_cap = 4 * ((len + 2) / 3) + 1;
    char *out = malloc((size_t)out_cap);
    int n = EVP_EncodeBlock((unsigned char *)out, src, len);
    out[n] = '\0';
    return out;
}

/* Commit: serialise edits into JSON body, POST to the server, parse the
 * response. Returns the new revision number or -1 on any failure. Frees
 * the builder on the way out. */
int
svnae_ra_commit_finish(struct svnae_ra_commit *cb,
                      const char *base_url, const char *repo_name)
{
    if (!cb) return -1;

    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "base_rev", cb->base_rev);
    cJSON_AddStringToObject(body, "author",   cb->author);
    cJSON_AddStringToObject(body, "log",      cb->logmsg);

    cJSON *jedits = cJSON_AddArrayToObject(body, "edits");
    for (int i = 0; i < cb->n; i++) {
        cJSON *e = cJSON_CreateObject();
        if (cb->edits[i].op == 1) {
            cJSON_AddStringToObject(e, "op", "add-file");
            cJSON_AddStringToObject(e, "path", cb->edits[i].path);
            char *b64 = b64_encode(cb->edits[i].content, cb->edits[i].content_len);
            cJSON_AddStringToObject(e, "content", b64);
            free(b64);
        } else if (cb->edits[i].op == 2) {
            cJSON_AddStringToObject(e, "op", "mkdir");
            cJSON_AddStringToObject(e, "path", cb->edits[i].path);
        } else if (cb->edits[i].op == 3) {
            cJSON_AddStringToObject(e, "op", "delete");
            cJSON_AddStringToObject(e, "path", cb->edits[i].path);
        }
        cJSON_AddItemToArray(jedits, e);
    }

    /* Add props object: { path: {key: value, ...}, ... }. */
    if (cb->n_props > 0) {
        cJSON *jp = cJSON_AddObjectToObject(body, "props");
        for (int i = 0; i < cb->n_props; i++) {
            const char *p = cb->props[i].path;
            cJSON *per = cJSON_GetObjectItemCaseSensitive(jp, p);
            if (!per) per = cJSON_AddObjectToObject(jp, p);
            cJSON_AddStringToObject(per, cb->props[i].key, cb->props[i].value);
        }
    }

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char url[1024];
    snprintf(url, sizeof url, "%s/repos/%s/commit", base_url, repo_name);

    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, json, &resp, &len, &status);
    free(json);

    int new_rev = -1;
    if (rc == 0 && status == 200 && resp) {
        cJSON *root = cJSON_ParseWithLength(resp, len);
        if (root) {
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "rev");
            if (cJSON_IsNumber(jr)) new_rev = jr->valueint;
            cJSON_Delete(root);
        }
    }
    free(resp);

    /* Free the builder's contents. */
    free(cb->author);
    free(cb->logmsg);
    for (int i = 0; i < cb->n; i++) {
        free(cb->edits[i].path);
        free(cb->edits[i].content);
    }
    for (int i = 0; i < cb->n_props; i++) {
        free(cb->props[i].path);
        free(cb->props[i].key);
        free(cb->props[i].value);
    }
    free(cb->edits);
    free(cb->props);
    free(cb);

    return new_rev;
}
