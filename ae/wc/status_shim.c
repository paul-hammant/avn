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

/* ae/wc/status_shim.c — svn status.
 *
 * Compares the on-disk tree under $wc against what wc.db thinks is there
 * plus the pristine store. Produces a list of status entries:
 *
 *   path, status_code
 *     ' '  Normal (tracked, unchanged)                — filtered from output
 *     'A'  Added (scheduled for commit, no base)
 *     'D'  Deleted (scheduled for commit, was tracked)
 *     'M'  Modified (tracked, disk bytes differ from pristine)
 *     '!'  Missing (tracked, gone from disk)
 *     '?'  Unversioned (on disk, not tracked, not .svn/)
 *
 * Algorithm:
 *   1. Walk wc.db rows. For each:
 *        - kind=file, state=normal: read disk file, sha1 it,
 *          compare to base_sha1 → Normal or Modified; if disk missing → Missing.
 *        - kind=file, state=added:    'A'
 *        - kind=file, state=deleted:  'D'
 *        - kind=dir,  state=normal: ensure dir exists; Missing if not.
 *   2. Walk the filesystem. For each file/dir not in the db and not under
 *      .svn/, emit '?'.
 *
 * We only return changed entries in the API (filter Normal here). Output
 * order is lexicographic path.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Externs from neighbouring shims. */
sqlite3     *svnae_wc_db_open(const char *wc_root);
void         svnae_wc_db_close(sqlite3 *db);

char *svnae_wc_propget(const char *wc_root, const char *path, const char *name);
void  svnae_wc_props_free(char *s);

struct svnae_wc_nodelist;
struct svnae_wc_nodelist *svnae_wc_db_list_nodes(sqlite3 *db);
int         svnae_wc_nodelist_count(const struct svnae_wc_nodelist *L);
const char *svnae_wc_nodelist_path(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_kind(const struct svnae_wc_nodelist *L, int i);
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_state(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_conflicted(const struct svnae_wc_nodelist *L, int i);
void        svnae_wc_nodelist_free(struct svnae_wc_nodelist *L);

/* ---- helpers ---------------------------------------------------------- */

extern int svnae_wc_hash_file(const char *wc_root, const char *path, char *out);

static __thread const char *g_wc_root = NULL;

static int
sha1_of_file(const char *path, char out[65])
{
    if (!g_wc_root) return -1;
    return svnae_wc_hash_file(g_wc_root, path, out);
}

/* ---- result list ----------------------------------------------------- */

struct status_entry { char *path; char code; };
struct svnae_wc_statuslist { struct status_entry *items; int n; int cap; };

static void
add_entry(struct svnae_wc_statuslist *L, const char *path, char code)
{
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 8;
        L->items = realloc(L->items, (size_t)nc * sizeof *L->items);
        L->cap = nc;
    }
    L->items[L->n].path = strdup(path);
    L->items[L->n].code = code;
    L->n++;
}

static int
entry_cmp(const void *a, const void *b)
{
    return strcmp(((const struct status_entry *)a)->path,
                  ((const struct status_entry *)b)->path);
}

/* ---- filesystem walk for unversioned ---------------------------------- *
 *
 * Walks $wc on disk, skipping .svn/, and emits '?' for any path not
 * present in `tracked` (a simple strset of known paths from wc.db).
 */

struct strset { char **items; int n; int cap; };

static int
strset_has(const struct strset *s, const char *v)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->items[i], v) == 0) return 1;
    return 0;
}

static void
strset_add(struct strset *s, const char *v)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        s->items = realloc(s->items, (size_t)nc * sizeof *s->items);
        s->cap = nc;
    }
    s->items[s->n++] = strdup(v);
}

static void
strset_clear(struct strset *s)
{
    for (int i = 0; i < s->n; i++) free(s->items[i]);
    free(s->items);
    s->items = NULL; s->n = 0; s->cap = 0;
}

/* FFI helper for ae/wc/ignore.ae — plain fnmatch(3) without
 * FNM_PATHNAME, matching svn:ignore's semantics (basename-style
 * patterns; `*` may span `/` characters if the caller ever passes
 * a multi-segment name). */
int32_t
svnae_fnmatch_plain(const char *glob, const char *path)
{
    return fnmatch(glob, path, 0) == 0 ? 1 : 0;
}

/* matches_ignore has been ported to Aether (ae/wc/ignore.ae,
 * --emit=lib). The C side just forwards to the Aether entry. */
extern int32_t aether_ignore_matches(const char *patterns, const char *name);

static int
matches_ignore(const char *patterns, const char *name)
{
    if (!patterns || !*patterns || !name) return 0;
    return aether_ignore_matches(patterns, name);
}

static void
walk_unversioned(const char *wc_root, const char *rel,
                 const struct strset *tracked,
                 struct svnae_wc_statuslist *out)
{
    char dir_path[PATH_MAX];
    if (*rel) snprintf(dir_path, sizeof dir_path, "%s/%s", wc_root, rel);
    else      snprintf(dir_path, sizeof dir_path, "%s", wc_root);

    /* Fetch svn:ignore for this directory. An empty `rel` is the WC
     * root — the db stores its props under the empty string iff we
     * ever upserted a row for it. In practice we may not have one, so
     * propget returns NULL and no patterns apply. */
    char *ignore_prop = svnae_wc_propget(wc_root, rel, "svn:ignore");

    DIR *d = opendir(dir_path);
    if (!d) { svnae_wc_props_free(ignore_prop); return; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0'
            || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
        if (strcmp(e->d_name, ".svn") == 0) continue;

        /* Skip conflict sidecars — .mine and .r<N>. They'll be cleaned
         * up by `svn resolve`. */
        {
            size_t nl = strlen(e->d_name);
            if (nl > 5 && strcmp(e->d_name + nl - 5, ".mine") == 0) continue;
            if (nl > 2 && e->d_name[nl - 2] == '.' && e->d_name[nl - 1] == 'r') {
                /* just ".r" — probably not a sidecar; let through */
            }
            /* Pattern: name ends in ".rN" where N is all digits. */
            const char *dot = strrchr(e->d_name, '.');
            if (dot && dot[1] == 'r' && dot[2]) {
                int all_digits = 1;
                for (const char *q = dot + 2; *q; q++) {
                    if (*q < '0' || *q > '9') { all_digits = 0; break; }
                }
                if (all_digits) continue;
            }
        }

        char child_rel[PATH_MAX];
        if (*rel) snprintf(child_rel, sizeof child_rel, "%s/%s", rel, e->d_name);
        else      snprintf(child_rel, sizeof child_rel, "%s", e->d_name);

        char child_fs[PATH_MAX];
        snprintf(child_fs, sizeof child_fs, "%s/%s", wc_root, child_rel);
        struct stat st;
        if (lstat(child_fs, &st) != 0) continue;

        int is_tracked = strset_has(tracked, child_rel);
        int ignored = 0;
        if (!is_tracked && ignore_prop) {
            ignored = matches_ignore(ignore_prop, e->d_name);
        }

        if (S_ISDIR(st.st_mode)) {
            if (!is_tracked) {
                if (!ignored) add_entry(out, child_rel, '?');
                /* Don't recurse into unversioned dirs — reference svn
                 * reports the dir once (or suppresses it) and stops. */
            } else {
                walk_unversioned(wc_root, child_rel, tracked, out);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!is_tracked && !ignored) add_entry(out, child_rel, '?');
        }
    }
    closedir(d);
    svnae_wc_props_free(ignore_prop);
}

/* ---- public API ------------------------------------------------------ */

struct svnae_wc_statuslist *
svnae_wc_status(const char *wc_root)
{
    g_wc_root = wc_root;
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return NULL;

    struct svnae_wc_statuslist *out = calloc(1, sizeof *out);
    struct strset tracked = {0};

    /* Pass 1: every tracked node. */
    struct svnae_wc_nodelist *L = svnae_wc_db_list_nodes(db);
    int n = svnae_wc_nodelist_count(L);
    for (int i = 0; i < n; i++) {
        const char *rel = svnae_wc_nodelist_path(L, i);
        int kind  = svnae_wc_nodelist_kind(L, i);
        int state = svnae_wc_nodelist_state(L, i);
        int conflicted = svnae_wc_nodelist_conflicted(L, i);
        const char *base_sha = svnae_wc_nodelist_base_sha1(L, i);

        strset_add(&tracked, rel);

        char disk[PATH_MAX];
        snprintf(disk, sizeof disk, "%s/%s", wc_root, rel);

        struct stat st;
        int on_disk = (lstat(disk, &st) == 0);

        /* Conflicted trumps other states — show 'C' regardless. */
        if (conflicted) { add_entry(out, rel, 'C'); continue; }

        if (state == 1 /*added*/) { add_entry(out, rel, 'A'); continue; }
        if (state == 2 /*deleted*/) { add_entry(out, rel, 'D'); continue; }
        /* Normal: compare. */
        if (!on_disk) { add_entry(out, rel, '!'); continue; }
        if (kind == 1 /*dir*/) { continue; }
        /* Tracked file: hash it. */
        char cur[65];
        if (sha1_of_file(disk, cur) != 0) { add_entry(out, rel, '!'); continue; }
        if (strcmp(cur, base_sha) != 0) add_entry(out, rel, 'M');
    }
    svnae_wc_nodelist_free(L);

    /* Pass 2: filesystem walk for '?' entries. */
    walk_unversioned(wc_root, "", &tracked, out);

    strset_clear(&tracked);
    svnae_wc_db_close(db);

    qsort(out->items, (size_t)out->n, sizeof *out->items, entry_cmp);
    return out;
}

int         svnae_wc_statuslist_count(const struct svnae_wc_statuslist *L) { return L ? L->n : 0; }

const char *
svnae_wc_statuslist_path(const struct svnae_wc_statuslist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].path;
}

int
svnae_wc_statuslist_code(const struct svnae_wc_statuslist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return 0;
    return (int)(unsigned char)L->items[i].code;
}

void
svnae_wc_statuslist_free(struct svnae_wc_statuslist *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) free(L->items[i].path);
    free(L->items);
    free(L);
}
