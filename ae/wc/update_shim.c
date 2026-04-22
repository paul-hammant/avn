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

/* ae/wc/update_shim.c — svn update.
 *
 * Pulls remote state at target rev into a checked-out WC:
 *   svnae_wc_update(wc_root, target_rev)
 *     target_rev == -1 means "use server's current head".
 *   Returns target_rev on success, -1 on error, -2 on conflict.
 *
 * Algorithm (non-incremental — we fetch the full tree listing):
 *   1. Pull the full remote tree listing recursively at target_rev.
 *      Yields a set of (path, kind, sha1 [for files]).
 *   2. For each local node:
 *        - if remote has it at same sha1 → just refresh base_rev
 *        - if remote has it at different sha1 AND local is clean → overwrite
 *        - if remote has it at different sha1 AND local is modified → CONFLICT
 *        - if remote doesn't have it:
 *            - local state=normal → delete it locally (remote deleted)
 *            - local state=added → keep (user's work in progress)
 *   3. For each remote node not locally tracked:
 *        - fetch via RA cat, write to disk, pristine, db row.
 *
 * We scan conflicts in a dry-run pass before applying any changes, so
 * a conflict leaves the WC unchanged.
 */

#include <dirent.h>
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

/* Externs from neighbouring shims. */
sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int      svnae_wc_db_delete_node(sqlite3 *db, const char *path);
int      svnae_wc_db_set_info(sqlite3 *db, const char *key, const char *value);
int      svnae_wc_db_set_conflicted(sqlite3 *db, const char *path, int conflicted);

int      svnae_merge3_apply(const char *wc_path,
                            const char *base, int base_len, int base_rev,
                            const char *theirs, int theirs_len, int theirs_rev);

char    *svnae_wc_pristine_get(const char *wc_root, const char *sha1);
int      svnae_wc_pristine_size(const char *wc_root, const char *sha1);
void     svnae_wc_pristine_free(char *p);

struct svnae_wc_nodelist;
struct svnae_wc_nodelist *svnae_wc_db_list_nodes(sqlite3 *db);
int         svnae_wc_nodelist_count(const struct svnae_wc_nodelist *L);
const char *svnae_wc_nodelist_path(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_kind(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_base_rev(const struct svnae_wc_nodelist *L, int i);
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_state(const struct svnae_wc_nodelist *L, int i);
void        svnae_wc_nodelist_free(struct svnae_wc_nodelist *L);

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

struct svnae_ra_props;
struct svnae_ra_props *svnae_ra_get_props(const char *base_url, const char *repo_name,
                                          int rev, const char *path);
int         svnae_ra_props_count(const struct svnae_ra_props *P);
const char *svnae_ra_props_name (const struct svnae_ra_props *P, int i);
const char *svnae_ra_props_value(const struct svnae_ra_props *P, int i);
void        svnae_ra_props_free (struct svnae_ra_props *P);

int svnae_wc_propset(const char *wc_root, const char *path,
                     const char *name, const char *value);
int svnae_wc_propdel(const char *wc_root, const char *path, const char *name);

struct svnae_wc_proplist;
struct svnae_wc_proplist *svnae_wc_proplist(const char *wc_root, const char *path);
int         svnae_wc_proplist_count(const struct svnae_wc_proplist *L);
const char *svnae_wc_proplist_name (const struct svnae_wc_proplist *L, int i);
const char *svnae_wc_proplist_value(const struct svnae_wc_proplist *L, int i);
void        svnae_wc_proplist_free (struct svnae_wc_proplist *L);

/* --- hashing helpers ------------------------------------------------
 *
 * Phase 6.1: WC content addressing uses the configured algorithm, which
 * is read per-call from the WC's info table. We set `g_wc_root` at the
 * top of the public entry points so the helpers below can find it
 * without threading it through every internal signature. The algo
 * changes only at checkout time, so a thread-local pointer is safe. */

extern int svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out);
extern int svnae_wc_hash_file (const char *wc_root, const char *path, char *out);

static __thread const char *g_wc_root = NULL;

/* sha1_of_bytes is still used by walk_remote (populates the
 * remote_tree's per-file sha during RA fetch). The file-hashing
 * and atomic-write / mkdir-p helpers moved into the Aether apply
 * pass (ae/wc/update_apply.ae) so the C side no longer needs
 * them. */
static void
sha1_of_bytes(const char *data, int len, char out[65])
{
    out[0] = '\0';
    if (g_wc_root) svnae_wc_hash_bytes(g_wc_root, data, len, out);
}

/* --- remote tree map --------------------------------------------------- *
 *
 * We accumulate the remote tree into a small "remote node" list, keyed by
 * the repo-relative path. Files get their sha1 filled in from a ra_cat →
 * sha1_of_bytes; directories leave sha1 empty. We keep the fetched bytes
 * alongside so the apply pass can reuse them without re-fetching.
 */

struct remote_node {
    char *path;
    int   kind;    /* 0=file 1=dir */
    char  sha1[65];   /* hex-encoded hash; sized for sha256 */
    char *data;    /* malloc'd, only for files */
    int   data_len;
};

struct remote_tree {
    struct remote_node *items;
    int n, cap;
};

static void
rtree_add(struct remote_tree *rt,
          const char *path, int kind,
          const char *data, int data_len)
{
    if (rt->n == rt->cap) {
        int nc = rt->cap ? rt->cap * 2 : 16;
        rt->items = realloc(rt->items, (size_t)nc * sizeof *rt->items);
        rt->cap = nc;
    }
    struct remote_node *e = &rt->items[rt->n++];
    e->path = strdup(path);
    e->kind = kind;
    e->sha1[0] = '\0';
    if (kind == 0 && data) {
        e->data = malloc((size_t)data_len + 1);
        memcpy(e->data, data, (size_t)data_len);
        e->data[data_len] = '\0';
        e->data_len = data_len;
        sha1_of_bytes(data, data_len, e->sha1);
    } else {
        e->data = NULL;
        e->data_len = 0;
    }
}

static void
rtree_free(struct remote_tree *rt)
{
    for (int i = 0; i < rt->n; i++) {
        free(rt->items[i].path);
        free(rt->items[i].data);
    }
    free(rt->items);
}

static const struct remote_node *
rtree_find(const struct remote_tree *rt, const char *path)
{
    for (int i = 0; i < rt->n; i++)
        if (strcmp(rt->items[i].path, path) == 0) return &rt->items[i];
    return NULL;
}

/* Accessors for the Aether apply-pass port. Indexed by position in
 * the remote_tree list (0..rtree_count-1). The data pointer+length
 * pair works like std.fs's TLS buffer: valid for the lifetime of
 * the remote_tree, safe to pass directly to rep-store writes. */
int rtree_count(const struct remote_tree *rt) { return rt ? rt->n : 0; }
const char *rtree_path_at(const struct remote_tree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return "";
    return rt->items[i].path;
}
int rtree_kind_at(const struct remote_tree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return -1;
    return rt->items[i].kind;
}
const char *rtree_sha_at(const struct remote_tree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return "";
    return rt->items[i].sha1;
}
const char *rtree_data_at(const struct remote_tree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return "";
    return rt->items[i].data ? rt->items[i].data : "";
}
int rtree_data_len_at(const struct remote_tree *rt, int i) {
    if (!rt || i < 0 || i >= rt->n) return 0;
    return rt->items[i].data_len;
}

/* Linear scan by path (Aether needs this to decide "skip if already
 * seen in pass A"). Returns the index or -1. */
int rtree_find_by_path(const struct remote_tree *rt, const char *path) {
    if (!rt || !path) return -1;
    for (int i = 0; i < rt->n; i++) {
        if (strcmp(rt->items[i].path, path) == 0) return i;
    }
    return -1;
}

/* Recursive walk of the remote tree. Prefix is relative to repo root;
 * "" for root. */
static int
walk_remote(const char *base_url, const char *repo, int rev,
            const char *prefix, struct remote_tree *rt)
{
    struct svnae_ra_list *L = svnae_ra_list(base_url, repo, rev, prefix);
    if (!L) return -1;
    extern const char *aether_path_join_rel(const char *prefix, const char *name);
    int n = svnae_ra_list_count(L);
    for (int i = 0; i < n; i++) {
        const char *name = svnae_ra_list_name(L, i);
        const char *kind = svnae_ra_list_kind(L, i);
        const char *rel = aether_path_join_rel(prefix, name);

        if (strcmp(kind, "dir") == 0) {
            rtree_add(rt, rel, 1, NULL, 0);
            if (walk_remote(base_url, repo, rev, rel, rt) != 0) {
                svnae_ra_list_free(L); return -1;
            }
        } else {
            char *data = svnae_ra_cat(base_url, repo, rev, rel);
            if (!data) { svnae_ra_list_free(L); return -1; }
            int len = (int)strlen(data);
            rtree_add(rt, rel, 0, data, len);
            svnae_ra_free(data);
        }
    }
    svnae_ra_list_free(L);
    return 0;
}

/* Pull props for `rel` from the server at `rev`, overwriting any that
 * changed and removing any local props that the server no longer has.
 * Set of keys we're told about = "remote". We (a) propset each remote
 * key/value, then (b) propdel any local key not in the remote set.
 *
 * The WC proplist query is per-path; empty remote set means "server
 * has no props for this path", in which case every local prop on
 * this path gets deleted. */
static void
ingest_props(const char *base_url, const char *repo, int rev,
             const char *wc_root, const char *rel)
{
    struct svnae_ra_props *P = svnae_ra_get_props(base_url, repo, rev, rel);
    int rn = P ? svnae_ra_props_count(P) : 0;

    /* (a) overwrite / add from remote. */
    for (int j = 0; j < rn; j++) {
        svnae_wc_propset(wc_root, rel, svnae_ra_props_name(P, j),
                                        svnae_ra_props_value(P, j));
    }

    /* (b) for each local prop that isn't in the remote set, delete it. */
    struct svnae_wc_proplist *L = svnae_wc_proplist(wc_root, rel);
    if (L) {
        int ln = svnae_wc_proplist_count(L);
        for (int i = 0; i < ln; i++) {
            const char *lname = svnae_wc_proplist_name(L, i);
            int found = 0;
            for (int j = 0; j < rn; j++) {
                if (strcmp(lname, svnae_ra_props_name(P, j)) == 0) { found = 1; break; }
            }
            if (!found) svnae_wc_propdel(wc_root, rel, lname);
        }
        svnae_wc_proplist_free(L);
    }

    if (P) svnae_ra_props_free(P);
}

/* Aether-callable wrapper around ingest_props. The RA-props /
 * WC-proplist handles are opaque C structs that don't round-trip
 * cleanly through Aether FFI, so the apply-pass port calls this
 * wrapper once per path-that-changed. */
void
update_ingest_props(const char *base_url, const char *repo, int rev,
                    const char *wc_root, const char *rel)
{
    ingest_props(base_url, repo, rev, wc_root, rel);
}

/* Aether-callable disk-file hash. Returns a TLS scratch string with
 * the 65-byte hex (empty on failure). Mirrors the `sha1_of_file`
 * helper that's now pass-A-only. */
const char *
update_sha1_of_file(const char *wc_root, const char *path)
{
    static __thread char buf[65];
    buf[0] = '\0';
    if (svnae_wc_hash_file(wc_root, path, buf) != 0) buf[0] = '\0';
    return buf;
}

/* --- conflict detection + apply --------------------------------------- */

int
svnae_wc_update(const char *wc_root, int target_rev)
{
    g_wc_root = wc_root;
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;

    char *base_url = svnae_wc_db_get_info(db, "base_url");
    char *repo     = svnae_wc_db_get_info(db, "repo");
    if (!base_url || !repo) {
        svnae_wc_db_close(db);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        return -1;
    }

    if (target_rev < 0) {
        target_rev = svnae_ra_head_rev(base_url, repo);
        if (target_rev < 0) {
            svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
            svnae_wc_db_close(db); return -1;
        }
    }

    /* 1. Pull full remote tree at target_rev. */
    struct remote_tree rt = {0};
    if (walk_remote(base_url, repo, target_rev, "", &rt) != 0) {
        rtree_free(&rt);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        svnae_wc_db_close(db);
        return -1;
    }

    /* 2. Snapshot local nodes. */
    struct svnae_wc_nodelist *L = svnae_wc_db_list_nodes(db);
    int n_local = svnae_wc_nodelist_count(L);


    /* 3. + 4. The two-pass apply (local → remote diff, then
     * remote-only additions) is in Aether now
     * (ae/wc/update_apply.ae). The C side keeps walk_remote, the
     * remote_tree struct, the wc.db/nodelist handles, and the prop
     * ingest helper — everything with opaque-handle or curl-bound
     * state. */
    extern int aether_update_apply_pass_a(const char *wc_root,
                                          const char *base_url,
                                          const char *repo, int target_rev,
                                          const struct remote_tree *rt,
                                          const struct svnae_wc_nodelist *L,
                                          sqlite3 *db);
    extern void aether_update_apply_pass_b(const char *wc_root,
                                           const char *base_url,
                                           const char *repo, int target_rev,
                                           const struct remote_tree *rt,
                                           const struct svnae_wc_nodelist *L,
                                           sqlite3 *db);
    int had_conflict = aether_update_apply_pass_a(wc_root, base_url, repo,
                                                  target_rev, &rt, L, db);
    aether_update_apply_pass_b(wc_root, base_url, repo, target_rev, &rt, L, db);

    /* 5. Bump base_rev in info. */
    char buf[16]; snprintf(buf, sizeof buf, "%d", target_rev);
    svnae_wc_db_set_info(db, "base_rev", buf);

    svnae_wc_nodelist_free(L);
    rtree_free(&rt);
    svnae_wc_info_free(base_url);
    svnae_wc_info_free(repo);
    svnae_wc_db_close(db);
    /* Update applied (possibly with conflicts — the conflicted flag is
     * set on each such node for status/commit). We return target_rev
     * either way; callers can check status to see what needs resolving. */
    (void)had_conflict;
    return target_rev;
}

/* svn switch: relocate the WC to point at a different branch (or
 * different repo) at `target_rev` (−1 means the new location's head).
 * We rewrite the `url`/`base_url`/`repo` info rows first, then reuse
 * svnae_wc_update — the update algorithm is exactly what switch needs
 * (fetch remote tree, diff against local, apply with 3-way merge on
 * overlap). Nodes whose content sha1 matches the new tree get a free
 * ride; everything else goes through the merge / conflict pipeline. */
int
svnae_wc_switch(const char *wc_root,
                const char *new_base_url, const char *new_repo,
                int target_rev)
{
    g_wc_root = wc_root;
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;

    /* Rewrite info before handing off to update. full_url matches what
     * checkout wrote originally: "<base>/<repo>". */
    char full_url[2048];
    snprintf(full_url, sizeof full_url, "%s/%s", new_base_url, new_repo);
    svnae_wc_db_set_info(db, "url",      full_url);
    svnae_wc_db_set_info(db, "base_url", new_base_url);
    svnae_wc_db_set_info(db, "repo",     new_repo);
    svnae_wc_db_close(db);

    return svnae_wc_update(wc_root, target_rev);
}
