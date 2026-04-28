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
 * Mirrors repos/shim.c's query shape but over HTTP + JSON. Handles
 * returned here (svnae_ra_log, svnae_ra_list, svnae_ra_info) match
 * the server-side equivalents so callers can switch repository
 * sources without reshape.
 *
 * HTTP plumbing lives in ae/ra/http_client.ae (std.http.client v2);
 * URL builders in urls.ae; JSON parse/build in parse.ae and
 * commit_build.ae. The C side keeps the public ABI and the auth-
 * state TLS pointers (X-Svnae-User / X-Svnae-Superuser tokens). */

#include "aether_string.h"  /* aether_string_data / aether_string_length */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- HTTP plumbing ---------------------------------------------------- */

/* Per-process auth context — set once at CLI startup, attached to every
 * outgoing request. Placeholder auth: the client claims identity; the
 * server trusts it. Superuser token proves bypass rights via a shared
 * secret. */
static char *g_client_user = NULL;
static char *g_client_super_token = NULL;

/* URL builders ported to Aether (ae/ra/urls.ae). */
extern const char *aether_url_info(const char *base, const char *repo);
extern const char *aether_url_log(const char *base, const char *repo);
extern const char *aether_url_commit(const char *base, const char *repo);
extern const char *aether_url_rev_info(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_paths(const char *base, const char *repo, int rev);
extern const char *aether_url_rev_cat(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_list(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_props(const char *base, const char *repo, int rev, const char *path);
extern const char *aether_url_rev_blame(const char *base, const char *repo, int rev, const char *path);

void svnae_ra_set_user(const char *user) {
    free(g_client_user);
    g_client_user = user && *user ? strdup(user) : NULL;
}
void svnae_ra_set_superuser_token(const char *token) {
    free(g_client_super_token);
    g_client_super_token = token && *token ? strdup(token) : NULL;
}

/* Auth getters called from ae/ra/http_client.ae's send_with_auth.
 * Empty string when the corresponding state isn't set — the Aether
 * side checks string.length() before adding the header. */
const char *aether_ra_get_user(void) {
    return g_client_user ? g_client_user : "";
}
const char *aether_ra_get_super_token(void) {
    return g_client_super_token ? g_client_super_token : "";
}

/* The libcurl-using helpers moved to ae/ra/http_client.ae. The Aether
 * helper returns a std.http.client response handle (ptr) we read via
 * typed accessors — status, body, header — and then free.
 *
 * Only one C-side caller remains (svnae_ra_cat below); everything
 * else is fetched + parsed + handed back as a packed string by
 * ae/ra/fetch.ae. */
extern const void *aether_ra_http_get(const char *url);
extern int         aether_ra_http_response_status(const void *resp);
extern const char *aether_ra_http_response_body  (const void *resp);
extern void        aether_ra_http_response_free  (const void *resp);

/* GET `url`, return malloc'd body bytes iff status==200, NULL
 * otherwise. Embedded-NUL safe (uses aether_string_length, not
 * strlen). Used only by svnae_ra_cat, which needs a length-aware
 * binary slurp the Aether-side aether_http_get_200 can't provide. */
static char *
ra_get_200_bytes(const char *url)
{
    const void *resp = aether_ra_http_get(url);
    if (!resp) return NULL;
    int status = aether_ra_http_response_status(resp);
    if (status != 200) { aether_ra_http_response_free(resp); return NULL; }

    const char *src = aether_ra_http_response_body(resp);
    int n = (int)aether_string_length(src);
    const char *data = aether_string_data(src);
    char *b = malloc((size_t)n + 1);
    if (b) {
        if (n > 0) memcpy(b, data, (size_t)n);
        b[n] = '\0';
    }
    aether_ra_http_response_free(resp);
    return b;
}

/* ---- packed-record handle accessor families ------------------------ *
 *
 * log / paths / blame / info / props / list handle wrappers all
 * retired in Round 155 — moved to ae/ra/accessors.ae alongside
 * Round 154's repos/accessors.ae port. Aether refcount handles
 * the stable-pointer lifetime contract that pin_str used to
 * provide; the per-handle struct collapses to the underlying
 * packed AetherString. */

/* ---- cat ------------------------------------------------------------- *
 *
 * Returns malloc'd NUL-terminated body bytes (caller frees via
 * svnae_ra_free). Embedded NULs in binary blobs aren't reflected in
 * strlen — a length-aware variant will land when binary flows
 * end-to-end. */
char *
svnae_ra_cat(const char *base_url, const char *repo_name, int rev, const char *path)
{
    /* Skip leading '/' in the user path so URLs look clean. */
    while (*path == '/') path++;
    return ra_get_200_bytes(aether_url_rev_cat(base_url, repo_name, rev, path));
}

void svnae_ra_free(char *p) { free(p); }

/* svnae_ra_get_props / svnae_ra_list (the props + list handle
 * families) retired alongside log/paths/blame/info in Round 155 —
 * see ae/ra/accessors.ae. */

/* ---- commit ---------------------------------------------------------- *
 *
 * Builder pattern: begin → add_file/mkdir/delete/set_prop/acl_add →
 * finish (which serialises + POSTs). Three packed-string buffers
 * (edits/props/acls) parallel ra/parse.ae's "<count>\x02<rec>\x02..."
 * shape so commit_build.ae walks each once via string.split. b64-
 * encoding happens at add-file time so the buffer carries text only. */

struct svnae_ra_commit {
    int   base_rev;
    char *author;
    char *logmsg;
    char *edits_packed;   /* "<n>\x02<op>\x01<path>\x01<b64>\x02..."   */
    int   n_edits;
    char *props_packed;   /* "<n>\x02<path>\x01<key>\x01<value>\x02..." */
    int   n_props;
    char *acls_packed;    /* "<n>\x02<path>\x01<rule>\x02..."          */
    int   n_acls;
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

/* svnae_ra_commit_{add_file,mkdir,delete,acl_add,set_prop} +
 * pack_append retired in Round 146 — moved to ae/ra/commit_build.ae
 * via std.string. The struct + setters/n-getters below let .ae
 * read the current packed buffer, build the new one with
 * pack_append_, and stash it back. */

/* Setters for the three packed-record buffers. Aether-side
 * pack_append_ produces the new packed string; we strdup-into the
 * struct field (replacing any previous value). */
static void
set_packed_(char **slot, const char *str, int *count_slot)
{
    free(*slot);
    *slot = strdup(str ? str : "");
    /* Re-derive count from the new packed shape: leading "<n>\x02"
     * if non-empty, else 0. Keeps cb->n_* in sync without the
     * Aether caller having to do extra setter work. */
    if (*slot && **slot) {
        *count_slot = atoi(*slot);
    } else {
        *count_slot = 0;
    }
}
void svnae_ra_cb_set_edits_packed(struct svnae_ra_commit *cb, const char *str) {
    if (cb) set_packed_(&cb->edits_packed, str, &cb->n_edits);
}
void svnae_ra_cb_set_props_packed(struct svnae_ra_commit *cb, const char *str) {
    if (cb) set_packed_(&cb->props_packed, str, &cb->n_props);
}
void svnae_ra_cb_set_acls_packed(struct svnae_ra_commit *cb, const char *str) {
    if (cb) set_packed_(&cb->acls_packed, str, &cb->n_acls);
}
int svnae_ra_cb_n_edits(const struct svnae_ra_commit *cb) {
    return cb ? cb->n_edits : 0;
}
int svnae_ra_cb_n_props(const struct svnae_ra_commit *cb) {
    return cb ? cb->n_props : 0;
}
int svnae_ra_cb_n_acls(const struct svnae_ra_commit *cb) {
    return cb ? cb->n_acls : 0;
}

/* --- Aether-callable accessors -------------------------------------- *
 *
 * 14 typed getters collapsed to 6 packed-string + 3 count fields.
 * commit_build.ae walks each packed buffer with std.string.split. */
int         svnae_ra_cb_base_rev(const struct svnae_ra_commit *cb) {
    return cb ? cb->base_rev : 0;
}
const char *svnae_ra_cb_author(const struct svnae_ra_commit *cb) {
    return (cb && cb->author) ? cb->author : "";
}
const char *svnae_ra_cb_logmsg(const struct svnae_ra_commit *cb) {
    return (cb && cb->logmsg) ? cb->logmsg : "";
}
const char *svnae_ra_cb_edits_packed(const struct svnae_ra_commit *cb) {
    return (cb && cb->edits_packed) ? cb->edits_packed : "";
}
const char *svnae_ra_cb_props_packed(const struct svnae_ra_commit *cb) {
    return (cb && cb->props_packed) ? cb->props_packed : "";
}
const char *svnae_ra_cb_acls_packed(const struct svnae_ra_commit *cb) {
    return (cb && cb->acls_packed) ? cb->acls_packed : "";
}

/* Aether builds the JSON body string; C side takes over for HTTP POST
 * + response parse + builder free. Split this way because http_post_json
 * + libcurl + the TLS-aware resp buffer all live in C. */
extern const char *aether_ra_commit_build_body(const struct svnae_ra_commit *cb);

extern int aether_ra_post_for_rev(const char *url, const char *body, int expected_status);

int
svnae_ra_commit_finish(struct svnae_ra_commit *cb,
                      const char *base_url, const char *repo_name)
{
    if (!cb) return -1;

    const char *body_json = aether_ra_commit_build_body(cb);
    const char *url = aether_url_commit(base_url, repo_name);
    int new_rev = aether_ra_post_for_rev(url, body_json, 200);

    free(cb->author);
    free(cb->logmsg);
    free(cb->edits_packed);
    free(cb->props_packed);
    free(cb->acls_packed);
    free(cb);

    return new_rev;
}

/* ---- verify glue ----------------------------------------------------
 *
 * The Merkle-verify walk lives in ae/ra/verify.ae. The two C-side
 * struct allocators below back it:
 *   svnae_verify_counter:  (files, secondaries) accumulator
 *   svnae_verify_entries:  (name, kind_c, sha) tuples sortable by name
 * Storage in C since Aether can't allocate struct-of-arrays with
 * ergonomic field access from FFI. */

/* svnae_verify_counter_* retired in Round 147 — verify.ae now uses
 * a 2-element std.intarr (slot 0 = files, slot 1 = secondaries). */

/* svnae_verify_entries_* (struct + 7 ops + qsort comparator)
 * retired in Round 148. verify.ae uses three parallel Aether
 * containers (std.list of names, std.intarr of kinds, std.list of
 * shas) plus a stable insertion sort keyed on name. */
