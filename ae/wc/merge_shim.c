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

/* ae/wc/merge_shim.c — svn merge (linear rev-range forward merge).
 *
 * Scope:
 *   svnae_wc_merge(wc_root, source_url_path, revA, revB)
 *     Apply the file-tree diff between revA and revB (of source_url_path
 *     on the WC's home server) into the WC. Writes bytes, queues add/rm
 *     in wc.db so the next commit pushes them. Records the merged range
 *     in the svn:mergeinfo property on the WC's root tracked node (or
 *     a sentinel we upsert if the root isn't tracked).
 *
 * Scope limits for Phase 5.12:
 *   - No conflict resolution. If a local file is already modified
 *     (disk sha != pristine sha) we abort with -2 BEFORE changing anything.
 *   - Merge source + target are the full WC (not a subpath). Reference
 *     svn lets you merge into any directory; we always merge into "."
 *     and require the user to cd to the right place.
 *   - mergeinfo ranges are tracked as a simple "<source-path>:revA-revB"
 *     newline-separated list. No subtraction of prior merges yet — we
 *     just append. A follow-up phase can do range arithmetic.
 *
 * The WC's existing base_url/repo is read from wc.db info. The
 * source_url_path is relative to the repo root — e.g. "trunk".
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

/* Externs we use. */
sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int      svnae_wc_db_delete_node(sqlite3 *db, const char *path);
int      svnae_wc_db_node_exists(sqlite3 *db, const char *path);

struct svnae_wc_node;
struct svnae_wc_node *svnae_wc_db_get_node(sqlite3 *db, const char *path);
int         svnae_wc_node_kind(const struct svnae_wc_node *n);
const char *svnae_wc_node_base_sha1(const struct svnae_wc_node *n);
int         svnae_wc_node_base_rev(const struct svnae_wc_node *n);
void        svnae_wc_node_free(struct svnae_wc_node *n);

char *svnae_wc_db_get_info(sqlite3 *db, const char *key);
void  svnae_wc_info_free(char *s);

const char *svnae_wc_pristine_put(const char *wc_root, const char *data, int len);

int   svnae_ra_head_rev(const char *base_url, const char *repo_name);
struct svnae_ra_list;
struct svnae_ra_list *svnae_ra_list(const char *base_url, const char *repo_name, int rev, const char *path);
int         svnae_ra_list_count(const struct svnae_ra_list *L);
const char *svnae_ra_list_name(const struct svnae_ra_list *L, int i);
const char *svnae_ra_list_kind(const struct svnae_ra_list *L, int i);
void        svnae_ra_list_free(struct svnae_ra_list *L);
char       *svnae_ra_cat(const char *base_url, const char *repo_name, int rev, const char *path);
void        svnae_ra_free(char *p);

int   svnae_wc_propset(const char *wc_root, const char *path, const char *name, const char *value);
char *svnae_wc_propget(const char *wc_root, const char *path, const char *name);
void  svnae_wc_props_free(char *s);

int   svnae_wc_db_set_conflicted(sqlite3 *db, const char *path, int conflicted);
int   svnae_merge3_apply(const char *wc_path,
                         const char *base, int base_len, int base_rev,
                         const char *theirs, int theirs_len, int theirs_rev);

/* --- tiny helpers ---------------------------------------------------- */

extern int svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out);
extern int svnae_wc_hash_file (const char *wc_root, const char *path, char *out);

static __thread const char *g_wc_root = NULL;

/* sha1_of_bytes is still used by rt_add to populate each remote file
 * entry's sha1 during walk_remote. The file-hashing, mkdir-p, and
 * atomic-write helpers moved into the Aether apply pass
 * (ae/wc/merge_apply.ae). */
static void
sha1_of_bytes(const char *data, int len, char out[65])
{
    out[0] = '\0';
    if (g_wc_root) svnae_wc_hash_bytes(g_wc_root, data, len, out);
}

/* --- remote-tree snapshot (same as update_shim) ---------------------- */

struct rnode { char *path; int kind; char sha1[65]; char *data; int data_len; };
struct rtree { struct rnode *items; int n, cap; };

static void
rt_add(struct rtree *rt, const char *path, int kind, const char *data, int dlen)
{
    if (rt->n == rt->cap) {
        int nc = rt->cap ? rt->cap * 2 : 16;
        rt->items = realloc(rt->items, (size_t)nc * sizeof *rt->items);
        rt->cap = nc;
    }
    struct rnode *e = &rt->items[rt->n++];
    e->path = strdup(path);
    e->kind = kind;
    e->sha1[0] = '\0';
    if (kind == 0 && data) {
        e->data = malloc((size_t)dlen + 1);
        memcpy(e->data, data, (size_t)dlen);
        e->data[dlen] = '\0';
        e->data_len = dlen;
        sha1_of_bytes(data, dlen, e->sha1);
    } else {
        e->data = NULL;
        e->data_len = 0;
    }
}

static void
rt_free(struct rtree *rt)
{
    for (int i = 0; i < rt->n; i++) { free(rt->items[i].path); free(rt->items[i].data); }
    free(rt->items);
}

static const struct rnode *
rt_find(const struct rtree *rt, const char *path)
{
    for (int i = 0; i < rt->n; i++) if (strcmp(rt->items[i].path, path) == 0) return &rt->items[i];
    return NULL;
}

/* Accessors for the Aether apply-pass port. Mirror the ones in
 * update_shim.c (different struct name — `rtree` here vs
 * `remote_tree` there — but same fields). */
int mergert_count(const struct rtree *rt) { return rt ? rt->n : 0; }
const char *mergert_path_at(const struct rtree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return "";
    return rt->items[i].path;
}
int mergert_kind_at(const struct rtree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return -1;
    return rt->items[i].kind;
}
const char *mergert_sha_at(const struct rtree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return "";
    return rt->items[i].sha1;
}
const char *mergert_data_at(const struct rtree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return "";
    return rt->items[i].data ? rt->items[i].data : "";
}
int mergert_data_len_at(const struct rtree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return 0;
    return rt->items[i].data_len;
}
int mergert_find_by_path(const struct rtree *rt, const char *path) {
    if (!rt || !path) return -1;
    for (int i = 0; i < rt->n; i++) {
        if (strcmp(rt->items[i].path, path) == 0) return i;
    }
    return -1;
}

/* Walk a subtree under `source_path` at `rev` on the remote. The stored
 * paths are RELATIVE TO source_path — so a merge of source_path=trunk
 * yields entries like "main.c", "lib/util.c" which we can apply in the
 * current WC (which was checked out at repo root). */
/* Aether-callable wrappers for the opaque rtree in this
 * translation unit. See ae/wc/merge_walk.ae — the Aether walker
 * calls these to populate the struct. */
void svnae_mergert_add_dir(void *rt, const char *path) {
    rt_add((struct rtree *)rt, path, 1, NULL, 0);
}
void svnae_mergert_add_file(void *rt, const char *path,
                             const char *data, int data_len) {
    rt_add((struct rtree *)rt, path, 0, data, data_len);
}

/* Recursive walker ported to ae/wc/merge_walk.ae. */
extern int aether_merge_walk_remote(const char *base_url, const char *repo,
                                    int rev, const char *source_path,
                                    const char *sub_prefix, void *rt);

static int
walk_remote(const char *base_url, const char *repo, int rev,
            const char *source_path, const char *sub_prefix,
            struct rtree *rt)
{
    return aether_merge_walk_remote(base_url, repo, rev, source_path,
                                    sub_prefix, rt);
}

/* --- mergeinfo plumbing ---------------------------------------------- *
 *
 * The parse/add/cancel/emit pipeline for svn:mergeinfo has been ported to
 * Aether in ae/wc/mergeinfo.ae (compiled as --emit=lib). See the extern
 * and wrapper further down; the rest of this section has been removed.
 */


/* Mergeinfo parse+add+cancel+emit ported to Aether (ae/wc/mergeinfo.ae,
 * --emit=lib). The C side keeps only the property-store I/O. */
extern const char *aether_mergeinfo_add_range(const char *existing,
                                              const char *source,
                                              int lo, int hi, int reverse);

static int
mergeinfo_add_range(const char *wc_root, const char *source,
                    int lo, int hi, int reverse)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    if (!svnae_wc_db_node_exists(db, "")) {
        svnae_wc_db_upsert_node(db, "", 1 /*dir*/, 0, "", 0);
    }
    svnae_wc_db_close(db);

    char *existing = svnae_wc_propget(wc_root, "", "svn:mergeinfo");
    const char *existing_or_empty = existing ? existing : "";
    const char *new_val = aether_mergeinfo_add_range(existing_or_empty,
                                                     source, lo, hi, reverse);
    svnae_wc_props_free(existing);

    int rc;
    if (new_val && *new_val) {
        rc = svnae_wc_propset(wc_root, "", "svn:mergeinfo", new_val);
    } else {
        /* Empty result — delete the property entirely. */
        extern int svnae_wc_propdel(const char *wc_root, const char *path, const char *name);
        svnae_wc_propdel(wc_root, "", "svn:mergeinfo");
        rc = 0;
    }
    return rc;
}

/* --- main merge entry point ------------------------------------------ */

/* target_path: the in-WC directory to apply the changes into. Pass ""
 * for the WC root (bare "svn merge URL@a:b"). Otherwise something like
 * "src-branch" maps the source-relative paths onto that subdirectory.
 *
 * rev_a < rev_b:  forward merge — apply (source@A → source@B) onto WC.
 * rev_a > rev_b:  reverse merge — undo the changes introduced by
 *                 (source@rev_b → source@rev_a). Internally we swap
 *                 the tree endpoints so the apply pass always diffs
 *                 from "base" to "theirs"; the diff direction is what
 *                 makes forward vs reverse happen.
 * rev_a == rev_b: no-op, returns 0.
 */
int
svnae_wc_merge(const char *wc_root, const char *source_path, int rev_a, int rev_b,
               const char *target_path)
{
    g_wc_root = wc_root;
    if (rev_a == rev_b) return 0;
    if (!target_path) target_path = "";

    int reverse  = (rev_a > rev_b);
    /* After the swap, rev_base < rev_theirs always; the original
     * rev_a/rev_b are preserved only for mergeinfo bookkeeping. */
    int rev_base   = reverse ? rev_b : rev_a;
    int rev_theirs = reverse ? rev_a : rev_b;

    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    char *base_url = svnae_wc_db_get_info(db, "base_url");
    char *repo     = svnae_wc_db_get_info(db, "repo");
    svnae_wc_db_close(db);
    if (!base_url || !repo) {
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        return -1;
    }

    /* Fetch the source tree at both endpoints. A is the "base" tree
     * (what gets diffed away); B is the "theirs" tree (what gets
     * applied). For a forward merge A=rev_a, B=rev_b. For reverse
     * merge A=rev_b, B=rev_a — so undoing a change means the pre-change
     * tree is "theirs" and replaces the post-change tree. */
    struct rtree A = {0}, B = {0};
    if (walk_remote(base_url, repo, rev_base,   source_path, "", &A) != 0 ||
        walk_remote(base_url, repo, rev_theirs, source_path, "", &B) != 0) {
        rt_free(&A); rt_free(&B);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        return -1;
    }
    if (reverse) {
        /* Swap so the apply pass below uses (base=A=post-change,
         * theirs=B=pre-change). The semantics are: file that exists
         * in "theirs" but not in "base" is ADDED to the WC (undoing a
         * server-side delete); file that exists in "base" but not
         * in "theirs" is REMOVED from the WC (undoing a server-side
         * add); file that differs between them is text-merged with
         * the pre-change content as the incoming text. */
        struct rtree tmp = A; A = B; B = tmp;
    }

    /* Compute the diff and apply: pass 1 deletions, pass 2 adds+mods.
     * Ported to ae/wc/merge_apply.ae. The C side keeps walk_remote
     * + rtree + sqlite handles. */
    db = svnae_wc_db_open(wc_root);

    extern void aether_merge_apply_deletions(const char *wc_root,
                                             const char *target_path,
                                             const struct rtree *a_rt,
                                             const struct rtree *b_rt,
                                             sqlite3 *db);
    extern void aether_merge_apply_adds_mods(const char *wc_root,
                                             const char *target_path,
                                             int rev_a, int rev_b,
                                             const struct rtree *a_rt,
                                             const struct rtree *b_rt,
                                             sqlite3 *db);
    aether_merge_apply_deletions(wc_root, target_path, &A, &B, db);
    aether_merge_apply_adds_mods(wc_root, target_path, rev_a, rev_b, &A, &B, db);

    svnae_wc_db_close(db);
    rt_free(&A); rt_free(&B);
    svnae_wc_info_free(base_url); svnae_wc_info_free(repo);

    /* Record merged range in svn:mergeinfo on the WC root. We always
     * store as (lo, hi) with a reverse flag — the line emitted gets
     * the leading '-' when reverse == 1. */
    mergeinfo_add_range(wc_root, source_path, rev_base, rev_theirs, reverse);
    return 0;
}
