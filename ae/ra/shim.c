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

/* ra/shim.c — libsvn_ra: REST client to aether-svnserver.
 *
 * Mirrors repos/shim.c's query shape, but over HTTP + JSON. The handles
 * returned here (svnae_ra_log, svnae_ra_list, svnae_ra_info) look and
 * behave the same as their server-side counterparts so Aether callers
 * that already know the repos query API can switch repository sources
 * without reshape.
 *
 * We do the HTTP work ourselves via libcurl rather than going through
 * std.http, because Aether can't model the function-pointer chains
 * cleanly and std.http's `get` wrapper is Aether-only. JSON is handled
 * by Aether's std.json — the cJSON_* spellings below are a thin compat
 * layer (see the macros that immediately follow the #include).
 *
 * Dependency: libcurl. Installed via `apt install libcurl4-openssl-dev`.
 */

#include "aether_json.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- cJSON → std.json compatibility layer -----------------------------
 *
 * This file started on cJSON (via -lcjson). Aether 0.79.0 ships
 * std.json with object-key iteration in addition to the previous
 * parser/builder surface, so every cJSON call-site maps to a std.json
 * equivalent. The macros + inlines below let the existing code read
 * naturally while running on the std.json backend.
 *
 * Differences worth calling out:
 *   - cJSON exposes struct fields (valueint, valuestring). std.json
 *     uses accessor functions. We introduce local inline helpers to
 *     keep the old expressions readable.
 *   - cJSON_ArrayForEach iterates via ->next. std.json is index-based.
 *     Our macro expands to a for-loop over json_array_size.
 *   - Object iteration (cJSON's p->child / p->next) maps to a macro
 *     that calls json_object_size_raw + json_object_{key,value}_at.
 *   - Builders: cJSON_Create* / cJSON_Add*ToObject map to json_create_*
 *     + json_object_set_raw.
 */
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

/* Index-based stand-in for cJSON_ArrayForEach(entry, arr). */
#define cJSON_ArrayForEach(entry, arr)                                 \
    for (int _ra_idx = 0,                                              \
             _ra_len = json_array_size(arr);                           \
         _ra_idx < _ra_len &&                                          \
         ((entry) = json_array_get_raw((arr), _ra_idx), 1);            \
         _ra_idx++)

/* Object-key iteration (new in Aether 0.79.0). `key_var` is const char*,
 * `val_var` is JsonValue*. */
#define cJSON_ObjectForEach(key_var, val_var, obj)                     \
    for (int _ro_idx = 0,                                              \
             _ro_len = json_object_size_raw(obj);                      \
         _ro_idx < _ro_len &&                                          \
         ((key_var) = json_object_key_at((obj), _ro_idx),              \
          (val_var) = json_object_value_at((obj), _ro_idx), 1);        \
         _ro_idx++)

/* --- builders used by the commit-body writer ----------------------
 *
 * IMPORTANT ownership note: std.json's json_object_set_raw /
 * json_array_add_raw deep-copy the passed value into the container's
 * arena and free the original. The old cJSON_Add*ToObject variants
 * below therefore can NOT return the passed-in pointer — they have to
 * re-fetch the copied value out of the container so the caller gets
 * a pointer they can keep using. */

static inline JsonValue *cJSON_CreateObject(void) { return json_create_object(); }
static inline JsonValue *cJSON_CreateString(const char *s) { return json_create_string(s); }

static inline int cJSON_AddItemToArray(JsonValue *arr, JsonValue *v) {
    return json_array_add_raw(arr, v);
}

static inline JsonValue *cJSON_AddNumberToObject(JsonValue *obj, const char *key, double n) {
    JsonValue *v = json_create_number(n);
    if (!v) return NULL;
    if (!json_object_set_raw(obj, key, v)) return NULL;
    return json_object_get_raw(obj, key);
}

static inline JsonValue *cJSON_AddStringToObject(JsonValue *obj, const char *key, const char *s) {
    JsonValue *v = json_create_string(s);
    if (!v) return NULL;
    if (!json_object_set_raw(obj, key, v)) return NULL;
    return json_object_get_raw(obj, key);
}

static inline JsonValue *cJSON_AddObjectToObject(JsonValue *obj, const char *key) {
    JsonValue *v = json_create_object();
    if (!v) return NULL;
    if (!json_object_set_raw(obj, key, v)) return NULL;
    return json_object_get_raw(obj, key);
}

static inline JsonValue *cJSON_AddArrayToObject(JsonValue *obj, const char *key) {
    JsonValue *v = json_create_array();
    if (!v) return NULL;
    if (!json_object_set_raw(obj, key, v)) return NULL;
    return json_object_get_raw(obj, key);
}

static inline char *cJSON_PrintUnformatted(JsonValue *v) {
    return json_stringify_raw(v);
}

/* ---- HTTP plumbing ---------------------------------------------------- */

/* Per-process auth context — set once at CLI startup, attached to every
 * outgoing request. Placeholder auth: the client claims identity; the
 * server trusts it. Superuser token proves bypass rights via a shared
 * secret. */
static char *g_client_user = NULL;
static char *g_client_super_token = NULL;

/* URL builders ported to Aether (ae/ra/urls.ae). */
extern const char *aether_url_rev_info(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_paths(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_cat(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_list(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_props(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_blame(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_branches_create(const char *base, const char *repo, const char *branch_name);
extern const char *aether_url_info(const char *base, const char *repo);
extern const char *aether_url_log(const char *base, const char *repo);
extern const char *aether_url_commit(const char *base, const char *repo);
extern const char *aether_url_copy(const char *base, const char *repo);
extern const char *aether_url_rev_hashes(const char *base, const char *repo, int rev, const char *path);

void svnae_ra_set_user(const char *user) {
    free(g_client_user);
    g_client_user = user && *user ? strdup(user) : NULL;
}
void svnae_ra_set_superuser_token(const char *token) {
    free(g_client_super_token);
    g_client_super_token = token && *token ? strdup(token) : NULL;
}

/* Environment-variable accessor for the Aether CLI. Returns a static
 * thread-local NUL-terminated string, or "" on miss. */
const char *svnae_env_get(const char *name) {
    static __thread char buf[512];
    const char *v = getenv(name);
    if (!v) { buf[0] = '\0'; return buf; }
    size_t n = strlen(v);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, v, n);
    buf[n] = '\0';
    return buf;
}

/* svnae_http_get_body lives below, after http_get is defined. */

/* Build a curl header list containing whatever auth we have. Caller
 * frees with curl_slist_free_all. Always returns non-NULL (at least
 * a User-Agent-less empty list is fine with libcurl's default). */
static struct curl_slist *
build_auth_headers(struct curl_slist *base)
{
    char buf[512];
    if (g_client_user) {
        snprintf(buf, sizeof buf, "X-Svnae-User: %s", g_client_user);
        base = curl_slist_append(base, buf);
    }
    if (g_client_super_token) {
        snprintf(buf, sizeof buf, "X-Svnae-Superuser: %s", g_client_super_token);
        base = curl_slist_append(base, buf);
    }
    return base;
}

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

/* Capture a specific response header into a buf. Matches are case-
 * insensitive; the first matching header wins. */
struct hdr_capture {
    const char *want_name;   /* canonical lower-case name the caller asked for */
    size_t      want_len;
    char       *value;       /* malloc'd, NUL-terminated */
};

static size_t
hdr_capture_cb(char *buffer, size_t size, size_t nitems, void *userdata)
{
    struct hdr_capture *c = userdata;
    size_t len = size * nitems;
    if (len < 2 || c->value) return len;  /* already captured or too short */
    /* Header line format: "Name: value\r\n" */
    const char *colon = memchr(buffer, ':', len);
    if (!colon) return len;
    size_t nlen = (size_t)(colon - buffer);
    if (nlen != c->want_len) return len;
    for (size_t i = 0; i < nlen; i++) {
        char a = buffer[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != c->want_name[i]) return len;
    }
    /* Skip the colon + any leading whitespace. */
    const char *v = colon + 1;
    size_t vlen = len - (size_t)(v - buffer);
    while (vlen > 0 && (*v == ' ' || *v == '\t')) { v++; vlen--; }
    while (vlen > 0 && (v[vlen-1] == '\r' || v[vlen-1] == '\n' || v[vlen-1] == ' '))
        vlen--;
    c->value = malloc(vlen + 1);
    if (c->value) { memcpy(c->value, v, vlen); c->value[vlen] = '\0'; }
    return len;
}

/* Perform GET. Returns (body, status). On success body is malloc'd NUL-
 * terminated; caller frees. On CURL-level failure body is NULL.
 * `out_node_hash` (nullable): if non-NULL, captures the
 * X-Svnae-Node-Hash response header, written to a new malloc'd string
 * the caller must free. Set to NULL when absent. */
/* Non-static so ae/client/verify_shim.c can call it without adding a
 * fourth RA accessor for every endpoint. */
int
http_get_ex(const char *url, char **out_body, size_t *out_len, int *out_status,
            char **out_node_hash)
{
    CURL *h = curl_easy_init();
    if (!h) return -1;
    struct buf b = {0};
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, buf_write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 30L);

    struct curl_slist *hdrs = build_auth_headers(NULL);
    if (hdrs) curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);

    struct hdr_capture cap = { "x-svnae-node-hash", 17, NULL };
    if (out_node_hash) {
        curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, hdr_capture_cb);
        curl_easy_setopt(h, CURLOPT_HEADERDATA, &cap);
    }

    CURLcode rc = curl_easy_perform(h);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) { free(b.data); free(cap.value); return -1; }

    *out_body = b.data;
    *out_len = b.len;
    *out_status = (int)status;
    if (out_node_hash) *out_node_hash = cap.value;
    return 0;
}

/* Back-compat thin wrapper. */
static int
http_get(const char *url, char **out_body, size_t *out_len, int *out_status)
{
    return http_get_ex(url, out_body, out_len, out_status, NULL);
}

/* Simple "fetch URL and return the body as a malloc'd string" used by
 * CLI subcommands that just want to print a JSON response verbatim.
 * Returns "" on any failure. Caller takes ownership. */
char *svnae_http_get_body(const char *url) {
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return strdup("");
    if (status != 200) { free(body); return strdup(""); }
    return body ? body : strdup("");
}

static int
http_post_json(const char *url, const char *body, char **out_resp, size_t *out_len, int *out_status)
{
    CURL *h = curl_easy_init();
    if (!h) return -1;
    struct buf b = {0};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = build_auth_headers(headers);
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
extern int aether_ra_parse_head_rev(const char *body);
int
svnae_ra_head_rev(const char *base_url, const char *repo_name)
{
    const char *url = aether_url_info(base_url, repo_name);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return -1;
    if (status != 200) { free(body); return -1; }
    /* JSON parse + head-field extraction ported to
     * ae/ra/parse.ae::ra_parse_head_rev. */
    int rev = aether_ra_parse_head_rev(body);
    free(body);
    return rev;
}

/* Query the server's primary content-address algorithm. Returns a
 * malloc'd string (caller frees via free()) or NULL on failure. If
 * the server predates Phase 6.1 and omits the field, returns "sha1"
 * so callers can safely default. */
extern const char *aether_ra_parse_hash_algo(const char *body);
char *
svnae_ra_hash_algo(const char *base_url, const char *repo_name)
{
    const char *url = aether_url_info(base_url, repo_name);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }
    /* JSON parse ported to ae/ra/parse.ae::ra_parse_hash_algo.
     * Parser returns "" only on parse failure; defaults to "sha1"
     * when the field is absent (pre-Phase-6.1 servers). */
    const char *algo = aether_ra_parse_hash_algo(body);
    free(body);
    if (!algo || !*algo) return NULL;
    return strdup(algo);
}

/* ---- log handle (matches repos/shim.c accessor shape) ---------------- */

struct log_entry { int rev; char *author; char *date; char *msg; };
struct svnae_ra_log { struct log_entry *entries; int n; };

extern const char *aether_ra_parse_log(const char *body);
struct svnae_ra_log *
svnae_ra_log(const char *base_url, const char *repo_name)
{
    const char *url = aether_url_log(base_url, repo_name);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

    /* JSON parse ported to ae/ra/parse.ae::ra_parse_log.
     * Returns "<N>\x02<entry0>\x02<entry1>\x02..." where each
     * entry is "<rev>\x01<author>\x01<date>\x01<msg>". */
    const char *packed = aether_ra_parse_log(body);
    free(body);
    if (!packed || !*packed) return NULL;

    /* Pull the leading "<N>\x02" count. */
    const char *p = packed;
    const char *sep = strchr(p, 2);
    if (!sep) return NULL;
    char nbuf[32];
    size_t nlen = (size_t)(sep - p);
    if (nlen >= sizeof nbuf) nlen = sizeof nbuf - 1;
    memcpy(nbuf, p, nlen); nbuf[nlen] = '\0';
    int n = (int)strtol(nbuf, NULL, 10);
    p = sep + 1;

    struct svnae_ra_log *lg = calloc(1, sizeof *lg);
    lg->n = n;
    lg->entries = calloc((size_t)(n > 0 ? n : 1), sizeof *lg->entries);

    for (int i = 0; i < n; i++) {
        const char *entry_end = strchr(p, 2);
        if (!entry_end) entry_end = p + strlen(p);

        /* Split entry on \x01: rev, author, date, msg. */
        const char *f0 = p;
        const char *f1 = NULL, *f2 = NULL, *f3 = NULL;
        for (const char *q = p; q < entry_end; q++) {
            if (*q == 1) {
                if      (!f1) f1 = q + 1;
                else if (!f2) f2 = q + 1;
                else if (!f3) f3 = q + 1;
            }
        }
        if (!f1 || !f2 || !f3) continue;  /* malformed — leave zeroed */

        char rbuf[32];
        size_t rlen = (size_t)(f1 - 1 - f0);
        if (rlen >= sizeof rbuf) rlen = sizeof rbuf - 1;
        memcpy(rbuf, f0, rlen); rbuf[rlen] = '\0';
        lg->entries[i].rev    = (int)strtol(rbuf, NULL, 10);
        lg->entries[i].author = strndup(f1, (size_t)(f2 - 1 - f1));
        lg->entries[i].date   = strndup(f2, (size_t)(f3 - 1 - f2));
        lg->entries[i].msg    = strndup(f3, (size_t)(entry_end - f3));

        p = *entry_end == 2 ? entry_end + 1 : entry_end;
    }
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

/* ---- paths-changed handle (for `svn log --verbose`) ----------------- */

struct path_change_ra { char action; char *path; };
struct svnae_ra_paths { struct path_change_ra *items; int n; };

struct svnae_ra_paths *
svnae_ra_paths_changed(const char *base_url, const char *repo_name, int rev)
{
    const char *url = aether_url_rev_paths(base_url, repo_name, rev);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return NULL;
    cJSON *jentries = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(jentries)) { cJSON_Delete(root); return NULL; }

    int n = cJSON_GetArraySize(jentries);
    struct svnae_ra_paths *P = calloc(1, sizeof *P);
    P->n = n;
    P->items = calloc((size_t)(n > 0 ? n : 1), sizeof *P->items);
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, jentries) {
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(e, "action");
        cJSON *jp = cJSON_GetObjectItemCaseSensitive(e, "path");
        P->items[i].action = (cJSON_IsString(ja) && json_valuestring(ja)[0])
                                 ? json_valuestring(ja)[0] : '?';
        P->items[i].path   = cJSON_IsString(jp) ? strdup(json_valuestring(jp)) : strdup("");
        i++;
    }
    cJSON_Delete(root);
    return P;
}

int svnae_ra_paths_count(const struct svnae_ra_paths *P) { return P ? P->n : 0; }

const char *
svnae_ra_paths_action(const struct svnae_ra_paths *P, int i)
{
    static const char a_[] = "A", m_[] = "M", d_[] = "D", u_[] = "";
    if (!P || i < 0 || i >= P->n) return u_;
    switch (P->items[i].action) {
        case 'A': return a_;
        case 'M': return m_;
        case 'D': return d_;
        default:  return u_;
    }
}

const char *
svnae_ra_paths_path(const struct svnae_ra_paths *P, int i)
{
    if (!P || i < 0 || i >= P->n) return "";
    return P->items[i].path;
}

void
svnae_ra_paths_free(struct svnae_ra_paths *P)
{
    if (!P) return;
    for (int i = 0; i < P->n; i++) free(P->items[i].path);
    free(P->items);
    free(P);
}

/* ---- blame handle (Phase 7.6) --------------------------------------- */

struct blame_line_ra { int rev; char *author; char *text; };
struct svnae_ra_blame { struct blame_line_ra *items; int n; };

struct svnae_ra_blame *
svnae_ra_blame(const char *base_url, const char *repo_name,
              int rev, const char *path)
{
    const char *url = aether_url_rev_blame(base_url, repo_name, rev, path);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return NULL;
    cJSON *jlines = cJSON_GetObjectItemCaseSensitive(root, "lines");
    if (!cJSON_IsArray(jlines)) { cJSON_Delete(root); return NULL; }

    int n = cJSON_GetArraySize(jlines);
    struct svnae_ra_blame *B = calloc(1, sizeof *B);
    B->n = n;
    B->items = calloc((size_t)(n > 0 ? n : 1), sizeof *B->items);
    int i = 0;
    cJSON *e;
    cJSON_ArrayForEach(e, jlines) {
        cJSON *jr = cJSON_GetObjectItemCaseSensitive(e, "rev");
        cJSON *ja = cJSON_GetObjectItemCaseSensitive(e, "author");
        cJSON *jt = cJSON_GetObjectItemCaseSensitive(e, "text");
        B->items[i].rev    = cJSON_IsNumber(jr) ? json_valueint(jr) : -1;
        B->items[i].author = cJSON_IsString(ja) ? strdup(json_valuestring(ja)) : strdup("");
        B->items[i].text   = cJSON_IsString(jt) ? strdup(json_valuestring(jt)) : strdup("");
        i++;
    }
    cJSON_Delete(root);
    return B;
}

int         svnae_ra_blame_count(const struct svnae_ra_blame *B) { return B ? B->n : 0; }
int         svnae_ra_blame_rev  (const struct svnae_ra_blame *B, int i) {
    return (B && i >= 0 && i < B->n) ? B->items[i].rev : -1;
}
const char *svnae_ra_blame_author(const struct svnae_ra_blame *B, int i) {
    return (B && i >= 0 && i < B->n) ? B->items[i].author : "";
}
const char *svnae_ra_blame_text(const struct svnae_ra_blame *B, int i) {
    return (B && i >= 0 && i < B->n) ? B->items[i].text : "";
}
void svnae_ra_blame_free(struct svnae_ra_blame *B) {
    if (!B) return;
    for (int i = 0; i < B->n; i++) { free(B->items[i].author); free(B->items[i].text); }
    free(B->items);
    free(B);
}

/* ---- info handle ----------------------------------------------------- */

struct svnae_ra_info { int rev; char *author; char *date; char *msg; char *root; };

extern const char *aether_ra_parse_info_rev(const char *body);
struct svnae_ra_info *
svnae_ra_info_rev(const char *base_url, const char *repo_name, int rev)
{
    const char *url = aether_url_rev_info(base_url, repo_name, rev);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }

    /* JSON parse ported to ae/ra/parse.ae::ra_parse_info_rev,
     * which returns a packed "<rev>\x01<author>\x01<date>\x01<msg>\x01<root>"
     * string. Split into the struct fields here. */
    const char *packed = aether_ra_parse_info_rev(body);
    free(body);
    if (!packed || !*packed) return NULL;

    struct svnae_ra_info *I = calloc(1, sizeof *I);
    const char *p = packed;
    const char *fields[5] = {0};
    int n_fields = 0;
    fields[n_fields++] = p;
    while (*p) {
        if (*p == 1 && n_fields < 5) {
            fields[n_fields++] = p + 1;
        }
        p++;
    }
    /* p now points at the NUL terminator. Each field extends from
     * its start pointer to the next \x01 or (for the last field) to p. */
    size_t flens[5];
    for (int i = 0; i < 5; i++) {
        const char *end = (i + 1 < n_fields) ? fields[i + 1] - 1 : p;
        flens[i] = (size_t)(end - fields[i]);
    }
    /* rev is the first field, parsed as int. */
    char rev_buf[32];
    size_t rl = flens[0] < sizeof rev_buf - 1 ? flens[0] : sizeof rev_buf - 1;
    memcpy(rev_buf, fields[0], rl); rev_buf[rl] = '\0';
    I->rev    = (int)strtol(rev_buf, NULL, 10);
    I->author = strndup(fields[1], flens[1]);
    I->date   = strndup(fields[2], flens[2]);
    I->msg    = strndup(fields[3], flens[3]);
    I->root   = strndup(fields[4], flens[4]);
    return I;
}

int         svnae_ra_info_rev_num(const struct svnae_ra_info *I) { return I ? I->rev : -1; }
const char *svnae_ra_info_author (const struct svnae_ra_info *I) { return I ? I->author : ""; }
const char *svnae_ra_info_date   (const struct svnae_ra_info *I) { return I ? I->date : ""; }
const char *svnae_ra_info_msg    (const struct svnae_ra_info *I) { return I ? I->msg : ""; }
const char *svnae_ra_info_root   (const struct svnae_ra_info *I) { return I && I->root ? I->root : ""; }

void
svnae_ra_info_free(struct svnae_ra_info *I)
{
    if (!I) return;
    free(I->author); free(I->date); free(I->msg); free(I->root);
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
    const char *url = aether_url_rev_cat(base_url, repo_name, rev, path);
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
    const char *url = aether_url_rev_props(base_url, repo_name, rev, path);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get(url, &body, &len, &status) != 0) return NULL;
    if (status != 200) { free(body); return NULL; }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return NULL;
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return NULL; }

    int n = json_object_size_raw(root);
    if (n < 0) n = 0;
    struct svnae_ra_props *P = calloc(1, sizeof *P);
    P->n = n;
    P->items = calloc((size_t)(n > 0 ? n : 1), sizeof *P->items);
    for (int i = 0; i < n; i++) {
        const char *k = json_object_key_at(root, i);
        JsonValue  *v = json_object_value_at(root, i);
        P->items[i].name  = strdup(k ? k : "");
        P->items[i].value = cJSON_IsString(v) ? strdup(json_valuestring(v)) : strdup("");
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

    const char *url = aether_url_copy(base_url, repo_name);
    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, json, &resp, &len, &status);
    free(json);

    int new_rev = -1;
    if (rc == 0 && status == 200 && resp) {
        cJSON *root = cJSON_ParseWithLength(resp, len);
        if (root) {
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "rev");
            if (cJSON_IsNumber(jr)) new_rev = json_valueint(jr);
            cJSON_Delete(root);
        }
    }
    free(resp);
    return new_rev;
}

/* ---- branch create (Phase 8.2a) ------------------------------------- *
 *
 * POST /repos/{r}/branches/<NAME>/create with JSON
 *   { "base": "main", "include": ["src/**", "README.md"] }
 * Super-user only. Returns the new rev number, or -1 on failure.
 *
 * `includes_joined` is a single newline-separated string for ergonomic
 * passage from Aether (Aether ptr-array types are awkward). We split
 * here and pass as a JSON array. */
int
svnae_ra_branch_create(const char *base_url, const char *repo_name,
                      const char *name, const char *base,
                      const char *includes_joined)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "base", base);
    cJSON *arr = cJSON_AddArrayToObject(body, "include");
    const char *p = includes_joined ? includes_joined : "";
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (n > 0) {
            char *s = malloc(n + 1);
            memcpy(s, p, n); s[n] = '\0';
            cJSON_AddItemToArray(arr, cJSON_CreateString(s));
            free(s);
        }
        if (!eol) break;
        p = eol + 1;
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    const char *url = aether_url_branches_create(base_url, repo_name, name);
    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, json, &resp, &len, &status);
    free(json);

    int new_rev = -1;
    if (rc == 0 && status == 201 && resp) {
        cJSON *root = cJSON_ParseWithLength(resp, len);
        if (root) {
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "rev");
            if (cJSON_IsNumber(jr)) new_rev = json_valueint(jr);
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
    const char *url = aether_url_rev_list(base_url, repo_name, rev, path);
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
        L->items[i].name = cJSON_IsString(jn) ? strdup(json_valuestring(jn)) : strdup("");
        L->items[i].kind = (cJSON_IsString(jk) && strcmp(json_valuestring(jk), "dir") == 0) ? 'd' : 'f';
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

/* Per-path ACL rules collected for commit. Flat list of (path, rule)
 * like "+alice" / "-eve"; commit_finish groups into array-per-path. */
struct commit_acl { char *path; char *rule; };

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
    struct commit_acl *acls;
    int   n_acls;
    int   cap_acls;
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

/* Record an ACL rule for `path` on the pending commit. Multiple calls
 * with the same path accumulate into an ACL array on the wire. Clearing
 * a path's ACL is done by calling svnae_ra_commit_acl_clear, which
 * sends an empty array. */
int
svnae_ra_commit_acl_add(struct svnae_ra_commit *cb,
                        const char *path, const char *rule)
{
    if (!cb) return -1;
    if (cb->n_acls == cb->cap_acls) {
        int nc = cb->cap_acls ? cb->cap_acls * 2 : 8;
        struct commit_acl *p = realloc(cb->acls, (size_t)nc * sizeof *p);
        if (!p) return -1;
        cb->acls = p;
        cb->cap_acls = nc;
    }
    cb->acls[cb->n_acls].path = strdup(path);
    cb->acls[cb->n_acls].rule = strdup(rule);
    cb->n_acls++;
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

/* Consolidated in ae/ffi/openssl/shim.c. */
extern char *svnae_openssl_b64_encode(const unsigned char *src, int len);
#define b64_encode svnae_openssl_b64_encode

/* Commit: serialise edits into JSON body, POST to the server, parse the
 * response. Returns the new revision number or -1 on any failure. Frees
 * the builder on the way out. */
int
svnae_ra_commit_finish(struct svnae_ra_commit *cb,
                      const char *base_url, const char *repo_name)
{
    if (!cb) return -1;

    /* Build bottom-up: std.json's set/add deep-copy the value into the
     * container's arena and free the original. Composition therefore
     * has to finish every child before it's attached to a parent —
     * otherwise later modifications land on the freed original, not
     * on the (fresh) copy inside the parent. We compose each container
     * standalone and only attach to `body` at the very end. */

    /* edits array — populated before attaching to body. */
    cJSON *jedits = json_create_array();
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
        json_array_add_raw(jedits, e);
    }

    /* props object: { path: {key: value, ...}, ... } — group by path
     * into standalone per-path objects, then assemble the outer object. */
    cJSON *jp = NULL;
    if (cb->n_props > 0) {
        jp = json_create_object();
        for (int i = 0; i < cb->n_props; i++) {
            const char *p = cb->props[i].path;
            /* Accumulate per-path entries into a map-like structure.
             * Since we can't mutate children already attached to jp
             * (set_raw deep-copies), we build per-path dicts up front. */
            cJSON *per = json_object_get_raw(jp, p);
            if (!per) {
                /* Create a new per-path object, fill it with just
                 * this one key, then attach — subsequent props on the
                 * same path hit the slow path below. */
                per = json_create_object();
                cJSON_AddStringToObject(per, cb->props[i].key, cb->props[i].value);
                json_object_set_raw(jp, p, per);
            } else {
                /* Path already exists in jp. We need to add another
                 * key to it, but `per` points at the attached copy
                 * and set_raw on its grandparent doesn't persist
                 * grandchild modifications. Workaround: rebuild the
                 * whole per-path object with the old keys + new key
                 * and re-attach under the same path. This is O(K²) on
                 * props-per-path but K is tiny in practice. */
                cJSON *replacement = json_create_object();
                int keys_n = json_object_size_raw(per);
                for (int k = 0; k < keys_n; k++) {
                    const char *kk = json_object_key_at(per, k);
                    JsonValue  *vv = json_object_value_at(per, k);
                    cJSON_AddStringToObject(replacement, kk, json_valuestring(vv));
                }
                cJSON_AddStringToObject(replacement, cb->props[i].key, cb->props[i].value);
                json_object_set_raw(jp, p, replacement);
            }
        }
    }

    /* acl object: { path: ["+rule1", "-rule2", ...], ... } — same pattern. */
    cJSON *ja = NULL;
    if (cb->n_acls > 0) {
        ja = json_create_object();
        for (int i = 0; i < cb->n_acls; i++) {
            const char *p = cb->acls[i].path;
            cJSON *arr = json_object_get_raw(ja, p);
            if (!arr) {
                arr = json_create_array();
                json_array_add_raw(arr, json_create_string(cb->acls[i].rule));
                json_object_set_raw(ja, p, arr);
            } else {
                /* Rebuild the array with existing entries + new one. */
                cJSON *replacement = json_create_array();
                int arr_n = json_array_size(arr);
                for (int j = 0; j < arr_n; j++) {
                    JsonValue *rv = json_array_get_raw(arr, j);
                    json_array_add_raw(replacement, json_create_string(json_valuestring(rv)));
                }
                json_array_add_raw(replacement, json_create_string(cb->acls[i].rule));
                json_object_set_raw(ja, p, replacement);
            }
        }
    }

    /* Now assemble the outer body with everything pre-populated. */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "base_rev", cb->base_rev);
    cJSON_AddStringToObject(body, "author",   cb->author);
    cJSON_AddStringToObject(body, "log",      cb->logmsg);
    json_object_set_raw(body, "edits", jedits);
    if (jp) json_object_set_raw(body, "props", jp);
    if (ja) json_object_set_raw(body, "acl",   ja);

    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    const char *url = aether_url_commit(base_url, repo_name);

    char *resp = NULL; size_t len = 0; int status = 0;
    int rc = http_post_json(url, json, &resp, &len, &status);
    free(json);

    int new_rev = -1;
    if (rc == 0 && status == 200 && resp) {
        cJSON *root = cJSON_ParseWithLength(resp, len);
        if (root) {
            cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "rev");
            if (cJSON_IsNumber(jr)) new_rev = json_valueint(jr);
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
    for (int i = 0; i < cb->n_acls; i++) {
        free(cb->acls[i].path);
        free(cb->acls[i].rule);
    }
    free(cb->edits);
    free(cb->props);
    free(cb->acls);
    free(cb);

    return new_rev;
}
/* ae/client/verify_shim.c — Merkle verification of a remote repository.
 *
 * svnae_client_verify(base_url, repo, rev) walks the tree at `rev`
 * top-down, re-hashes every file content and dir blob locally using
 * the server's advertised algorithm, and confirms:
 *
 *   1. Each file/dir node's locally-computed content hash matches
 *      the X-Svnae-Node-Hash header the server returned.
 *   2. For each directory, the re-assembled blob (lines
 *      "<kind> <child-sha> <name>\n", sorted) hashes to the same
 *      sha that the parent dir pointed at.
 *   3. The root-dir sha equals the rev blob's "root" field
 *      (served via /info).
 *
 * The interesting move is step 2: the server gives us a JSON listing
 * of a directory's entries (name + kind), but to re-hash a dir we
 * need to reconstruct its *blob*, which has the wire format
 *   <kind-char> <child-sha> <name>\n
 * We obtain each child's sha by fetching that child (cat for files,
 * list for dirs) and reading the X-Svnae-Node-Hash header. Recurse.
 *
 * The walk is depth-first from the root. We bail on the first
 * mismatch and return a negative code; callers print a diagnostic.
 *
 * Returns:
 *    0 on full verification success
 *   -1 on I/O / protocol error
 *   -2 on hash mismatch (message printed to stderr with path + details)
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* ---- forward decls from the RA shim -------------------------------- */

struct svnae_ra_info;
struct svnae_ra_info *svnae_ra_info_rev(const char *base_url, const char *repo,
                                        int rev);
int         svnae_ra_info_rev_num(const struct svnae_ra_info *I);
const char *svnae_ra_info_root   (const struct svnae_ra_info *I);
void        svnae_ra_info_free   (struct svnae_ra_info *I);

char *svnae_ra_hash_algo(const char *base_url, const char *repo);

/* We need to fetch a node's raw bytes + its Merkle header.  The existing
 * RA accessors (svnae_ra_cat, svnae_ra_list) don't surface the header.
 * Rather than add a third variant to each, the verify pass speaks HTTP
 * directly via the RA shim's already-exposed http_get_ex (declared
 * here; defined in ra/shim.c). */
extern int http_get_ex(const char *url, char **out_body, size_t *out_len,
                       int *out_status, char **out_node_hash);

/* Hashing consolidated in ae/ffi/openssl/shim.c. */
extern char *svnae_openssl_hash_hex(const char *algo, const char *data, int len);
#define hash_hex svnae_openssl_hash_hex

/* Sort helper for dir entries so the re-assembled blob matches the
 * server's canonical line order. */
struct entry { char kind_c; char *name; char *sha; };

static int
entry_cmp(const void *a, const void *b)
{
    const struct entry *ea = a, *eb = b;
    return strcmp(ea->name, eb->name);
}

/* Fetch and verify a single file at `rel`. Returns 0 on match, -1 on
 * I/O error, -2 on hash mismatch. Out: `*out_sha` gets a malloc'd
 * copy of the computed sha (for the parent to thread into its own
 * blob). */
static int
verify_file(const char *base_url, const char *repo, int rev,
            const char *algo, const char *rel, char **out_sha)
{
    const char *url = aether_url_rev_cat(base_url, repo, rev, rel);
    char *body = NULL; size_t len = 0; int status = 0; char *hdr = NULL;
    if (http_get_ex(url, &body, &len, &status, &hdr) != 0 || status != 200) {
        free(body); free(hdr);
        fprintf(stderr, "verify: GET failed for %s (status %d)\n", rel, status);
        return -1;
    }
    /* Compute the file's content hash locally. Note: the server
     * currently sends NUL-terminated text and we strlen it; same
     * convention as `svnae_ra_cat`. When binary flows we'll switch
     * to len-aware. */
    int clen = (int)strlen(body);
    char *local = hash_hex(algo, body, clen);
    free(body);
    if (!local) { free(hdr); return -1; }

    if (!hdr || strcmp(local, hdr) != 0) {
        fprintf(stderr, "verify: hash mismatch at /%s\n", rel);
        fprintf(stderr, "  server: %s\n", hdr ? hdr : "(missing)");
        fprintf(stderr, "  local:  %s\n", local);
        free(hdr); free(local);
        return -2;
    }
    free(hdr);
    *out_sha = local;
    return 0;
}

/* Recursive dir verify. `rel` is the repo-relative path of the
 * directory being verified ("" for the root). The caller already
 * knows the sha the parent attributed to this dir; we compare it
 * against the dir's recomputed sha after recursion. */
static int
verify_dir(const char *base_url, const char *repo, int rev,
           const char *algo, const char *rel, char **out_sha)
{
    /* GET /rev/N/list/<path> — listing tells us the immediate children
     * (names + kinds). The response header carries the server's view
     * of *this* dir's blob sha. */
    const char *url = aether_url_rev_list(base_url, repo, rev, rel);

    char *body = NULL; size_t len = 0; int status = 0; char *dir_hdr = NULL;
    if (http_get_ex(url, &body, &len, &status, &dir_hdr) != 0 || status != 200) {
        free(body); free(dir_hdr);
        fprintf(stderr, "verify: GET /list failed for /%s (status %d)\n",
                rel, status);
        return -1;
    }

    /* Parse via std.json (the compat layer at the top of this file
     * maps cJSON_* names to json_*). */
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) { free(dir_hdr); return -1; }
    cJSON *jents = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(jents)) { cJSON_Delete(root); free(dir_hdr); return -1; }
    int n = cJSON_GetArraySize(jents);
    struct entry *entries = calloc((size_t)(n > 0 ? n : 1), sizeof *entries);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(jents, i);
        cJSON *jname = cJSON_GetObjectItemCaseSensitive(e, "name");
        cJSON *jkind = cJSON_GetObjectItemCaseSensitive(e, "kind");
        entries[i].name = strdup(cJSON_IsString(jname) ? json_valuestring(jname) : "");
        const char *kind = cJSON_IsString(jkind) ? json_valuestring(jkind) : "file";
        entries[i].kind_c = (kind[0] == 'd') ? 'd' : 'f';
        entries[i].sha = NULL;
    }
    cJSON_Delete(root);

    /* Recurse into every child to compute its sha. */
    extern const char *aether_path_join_rel(const char *prefix, const char *name);
    for (int i = 0; i < n; i++) {
        const char *child_rel = aether_path_join_rel(rel, entries[i].name);

        int rc;
        if (entries[i].kind_c == 'd') {
            rc = verify_dir (base_url, repo, rev, algo, child_rel, &entries[i].sha);
        } else {
            rc = verify_file(base_url, repo, rev, algo, child_rel, &entries[i].sha);
        }
        if (rc != 0) {
            for (int j = 0; j <= i; j++) { free(entries[j].name); free(entries[j].sha); }
            for (int j = i + 1; j < n; j++) { free(entries[j].name); }
            free(entries);
            free(dir_hdr);
            return rc;
        }
    }

    /* Rebuild the dir blob (sorted by name) and hash it. Line format:
     *   <kind> <sha> <name>\n
     * matching fs_fs/txn_shim.c:rebuild_dir_c's serialiser. */
    qsort(entries, (size_t)n, sizeof *entries, entry_cmp);
    size_t buf_cap = 1024, buf_len = 0;
    char *buf = malloc(buf_cap);
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        size_t need = 2 + strlen(entries[i].sha) + 1 + strlen(entries[i].name) + 2;
        if (buf_len + need + 1 >= buf_cap) {
            buf_cap = (buf_len + need + 1) * 2;
            buf = realloc(buf, buf_cap);
        }
        buf_len += (size_t)snprintf(buf + buf_len, buf_cap - buf_len,
                                    "%c %s %s\n",
                                    entries[i].kind_c, entries[i].sha, entries[i].name);
    }

    char *local = hash_hex(algo, buf, (int)buf_len);
    free(buf);

    /* Compare the re-hashed blob against the dir header the server
     * advertised for *this* dir. */
    int ok = (local && dir_hdr && strcmp(local, dir_hdr) == 0);
    if (!ok) {
        fprintf(stderr, "verify: dir hash mismatch at /%s\n", *rel ? rel : "");
        fprintf(stderr, "  server: %s\n", dir_hdr ? dir_hdr : "(missing)");
        fprintf(stderr, "  local:  %s\n", local ? local : "(null)");
    }
    free(dir_hdr);

    for (int i = 0; i < n; i++) { free(entries[i].name); free(entries[i].sha); }
    free(entries);

    if (!ok) { free(local); return -2; }
    *out_sha = local;
    return 0;
}

/* Crawl the tree rooted at `rel` and, for every file, fetch its
 * /hashes endpoint. For each secondary algo the server stores,
 * re-hash the content locally and compare. Returns 0 match, -1 I/O,
 * -2 mismatch. Emits a per-file OK summary to stdout with the
 * secondary-hash count when the caller asked for verbose output. */
static int
verify_secondaries_in_dir(const char *base_url, const char *repo, int rev,
                          const char *rel, int *out_files_checked,
                          int *out_secondary_count)
{
    const char *url = aether_url_rev_list(base_url, repo, rev, rel);
    char *body = NULL; size_t len = 0; int status = 0;
    if (http_get_ex(url, &body, &len, &status, NULL) != 0 || status != 200) {
        free(body);
        return -1;
    }
    /* Parse entries via std.json (same shape as verify_dir). */
    struct entry { char kind_c; char *name; };
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) return -1;
    cJSON *jents = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(jents)) { cJSON_Delete(root); return -1; }
    int n = cJSON_GetArraySize(jents);
    struct entry *ents = calloc((size_t)(n > 0 ? n : 1), sizeof *ents);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_GetArrayItem(jents, i);
        cJSON *jname = cJSON_GetObjectItemCaseSensitive(e, "name");
        cJSON *jkind = cJSON_GetObjectItemCaseSensitive(e, "kind");
        ents[i].name = strdup(cJSON_IsString(jname) ? json_valuestring(jname) : "");
        const char *kind = cJSON_IsString(jkind) ? json_valuestring(jkind) : "file";
        ents[i].kind_c = (kind[0] == 'd') ? 'd' : 'f';
    }
    cJSON_Delete(root);

    int overall = 0;
    extern const char *aether_path_join_rel(const char *prefix, const char *name);
    for (int i = 0; i < n; i++) {
        const char *child_rel = aether_path_join_rel(rel, ents[i].name);

        if (ents[i].kind_c == 'd') {
            int rc = verify_secondaries_in_dir(base_url, repo, rev, child_rel,
                                               out_files_checked, out_secondary_count);
            if (rc != 0 && overall == 0) overall = rc;
        } else {
            /* Fetch /cat and /hashes; re-hash for each declared secondary. */
            const char *cat_url = aether_url_rev_cat(base_url, repo, rev, child_rel);
            char *cbody = NULL; size_t clen = 0; int cstatus = 0;
            if (http_get_ex(cat_url, &cbody, &clen, &cstatus, NULL) != 0
                || cstatus != 200) {
                if (overall == 0) overall = -1;
                free(cbody); continue;
            }
            int body_len = (int)strlen(cbody);

            const char *h_url = aether_url_rev_hashes(base_url, repo, rev, child_rel);
            char *hbody = NULL; size_t hlen = 0; int hstatus = 0;
            if (http_get_ex(h_url, &hbody, &hlen, &hstatus, NULL) != 0
                || hstatus != 200) {
                free(cbody); free(hbody);
                if (overall == 0) overall = -1;
                continue;
            }

            /* Parse the secondaries array via std.json. */
            cJSON *hroot = cJSON_ParseWithLength(hbody, hlen);
            if (hroot) {
                cJSON *jsecs = cJSON_GetObjectItemCaseSensitive(hroot, "secondaries");
                if (cJSON_IsArray(jsecs)) {
                    int sec_n = cJSON_GetArraySize(jsecs);
                    for (int si = 0; si < sec_n; si++) {
                        cJSON *sec = cJSON_GetArrayItem(jsecs, si);
                        cJSON *jalgo = cJSON_GetObjectItemCaseSensitive(sec, "algo");
                        cJSON *jhash = cJSON_GetObjectItemCaseSensitive(sec, "hash");
                        if (!cJSON_IsString(jalgo) || !cJSON_IsString(jhash)) continue;
                        const char *algo = json_valuestring(jalgo);
                        const char *server_hex = json_valuestring(jhash);
                        char *local = hash_hex(algo, cbody, body_len);
                        if (local && strcmp(local, server_hex) == 0) {
                            (*out_secondary_count)++;
                        } else {
                            fprintf(stderr,
                                    "verify: secondary %s mismatch at /%s\n"
                                    "  server: %s\n  local:  %s\n",
                                    algo, child_rel,
                                    server_hex, local ? local : "(null)");
                            if (overall == 0) overall = -2;
                        }
                        free(local);
                    }
                }
                cJSON_Delete(hroot);
            }
            free(cbody); free(hbody);
            (*out_files_checked)++;
        }
    }
    for (int i = 0; i < n; i++) free(ents[i].name);
    free(ents);
    return overall;
}

/* Public entry point. Prints a short summary on success. Returns
 * 0 on match, -1 on protocol/IO error, -2 on Merkle mismatch.
 *
 * When `with_secondaries` is 1, after the primary Merkle walk passes
 * the tree is re-walked to verify every file's stored secondary
 * hashes. Mismatch in a secondary fails the whole verify. */
int
svnae_client_verify_full(const char *base_url, const char *repo, int rev,
                        int with_secondaries)
{
    char *algo = svnae_ra_hash_algo(base_url, repo);
    if (!algo || !*algo) { free(algo); fprintf(stderr, "verify: server info unavailable\n"); return -1; }

    struct svnae_ra_info *I = svnae_ra_info_rev(base_url, repo, rev);
    if (!I) { free(algo); fprintf(stderr, "verify: rev %d info unavailable\n", rev); return -1; }
    const char *claimed_root = svnae_ra_info_root(I);
    if (!*claimed_root) {
        svnae_ra_info_free(I); free(algo);
        fprintf(stderr, "verify: rev %d has no root sha in /info\n", rev);
        return -1;
    }
    char *claimed_root_copy = strdup(claimed_root);
    svnae_ra_info_free(I);

    char *root_sha = NULL;
    int rc = verify_dir(base_url, repo, rev, algo, "", &root_sha);
    if (rc != 0) {
        free(algo); free(root_sha); free(claimed_root_copy);
        return rc;
    }

    if (!root_sha || strcmp(root_sha, claimed_root_copy) != 0) {
        fprintf(stderr, "verify: root sha mismatch\n");
        fprintf(stderr, "  rev-blob root: %s\n", claimed_root_copy);
        fprintf(stderr, "  recomputed:    %s\n", root_sha ? root_sha : "(null)");
        free(algo); free(root_sha); free(claimed_root_copy);
        return -2;
    }

    int sec_rc = 0;
    int files = 0, sec_count = 0;
    if (with_secondaries) {
        sec_rc = verify_secondaries_in_dir(base_url, repo, rev, "", &files, &sec_count);
    }

    if (sec_rc != 0) {
        fprintf(stderr, "verify: secondary check failed (rc=%d)\n", sec_rc);
        free(algo); free(root_sha); free(claimed_root_copy);
        return sec_rc;
    }

    if (with_secondaries) {
        fprintf(stdout, "verify: OK (algo=%s, root=%s, %d file(s), %d secondary hash(es) verified)\n",
                algo, root_sha, files, sec_count);
    } else {
        fprintf(stdout, "verify: OK (algo=%s, root=%s)\n", algo, root_sha);
    }
    free(algo); free(root_sha); free(claimed_root_copy);
    return 0;
}

int
svnae_client_verify(const char *base_url, const char *repo, int rev)
{
    return svnae_client_verify_full(base_url, repo, rev, 0);
}
