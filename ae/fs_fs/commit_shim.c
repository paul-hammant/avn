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

/* fs_fs/commit_shim.c — finalise a txn into a revision.
 *
 * The recursive tree-rebuild lives in txn_shim.c (svnae_txn_rebuild_root).
 * This file wraps that with the last steps: build the revision blob,
 * write revs/NNNNNN pointer, bump $repo/head. A single entry point
 * callable both from Aether and from the HTTP server handler.
 *
 * Returns the new revision number on success, -1 on failure.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct svnae_txn;
struct svnae_paths;

/* Forward-declared from other shims we depend on. */
int   svnae_txn_base_rev(const struct svnae_txn *t);
char *svnae_txn_rebuild_root(const char *repo, const char *base_root_sha1,
                             const struct svnae_txn *t);
const char *svnae_rep_write_blob(const char *repo, const char *data, int len);
char       *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
void        svnae_rep_free(char *p);
const char *svnae_fsfs_now_iso8601(void);
int         svnae_repos_head_rev(const char *repo);

/* Atomic write + mkdir-p ported to Aether (ae/subr/io.ae, --emit=lib --with=fs). */
extern int aether_io_write_atomic(const char *path, const char *data, int length);
extern int aether_io_mkdir_p(const char *path);

static int write_atomic(const char *path, const char *data, int len)
{
    return aether_io_write_atomic(path, data, len) == 0 ? 0 : -1;
}

/* Read the root-dir sha1 for a given revision. Returns malloc'd string
 * or NULL. */
extern const char *aether_io_read_file(const char *path);
extern int aether_io_file_size(const char *path);

static char *
load_rev_root_sha1(const char *repo, int rev)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/revs/%06d", repo, rev);
    if (aether_io_file_size(path) < 0) return NULL;
    const char *src = aether_io_read_file(path);
    char buf[128];
    size_t slen = strlen(src);
    if (slen >= sizeof buf) slen = sizeof buf - 1;
    memcpy(buf, src, slen);
    buf[slen] = '\0';
    /* trim trailing whitespace */
    size_t n = slen;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = '\0';

    char *rev_body = svnae_rep_read_blob(repo, buf);
    if (!rev_body) return NULL;
    /* Parse "root: <sha1>" line. */
    const char *key = "root: ";
    char *p = strstr(rev_body, key);
    if (!p) { svnae_rep_free(rev_body); return NULL; }
    p += strlen(key);
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char *out = malloc(len + 1);
    memcpy(out, p, len);
    out[len] = '\0';
    svnae_rep_free(rev_body);
    return out;
}

/* Finalise: take the txn's base rev, rebuild the tree, write a new
 * revision blob, bump head, return the new rev number. -1 on failure.
 *
 * `props_sha1` is the sha1 of a paths-props blob associated with this
 * commit — pass an empty string to inherit the previous rev's props
 * (rep-sharing works automatically), or a specific sha1 when commits
 * touch properties. This extension is additive: older clients/tests
 * that pass "" always get the previous rev's props-sha1 carried
 * forward. */

/* Field extraction ported to Aether (ae/repos/blobfield.ae). */
extern const char *aether_blobfield_get(const char *body, const char *key);
extern const char *aether_rev_blob_body(const char *root, const char *branch,
                                        const char *props, const char *acl,
                                        int prev, const char *author,
                                        const char *date, const char *log);
extern const char *aether_paths_index_sort_by_path(const char *body);

static char *
rev_blob_field(const char *repo, int rev, const char *key)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/revs/%06d", repo, rev);
    if (aether_io_file_size(path) < 0) return NULL;
    const char *src = aether_io_read_file(path);
    char buf[128];
    size_t slen = strlen(src);
    if (slen >= sizeof buf) slen = sizeof buf - 1;
    memcpy(buf, src, slen);
    buf[slen] = '\0';
    size_t n = slen;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';

    char *body = svnae_rep_read_blob(repo, buf);
    if (!body) return NULL;
    const char *v = aether_blobfield_get(body, key);
    char *out = (v && *v) ? strdup(v) : NULL;
    svnae_rep_free(body);
    return out;
}

int
svnae_commit_finalise(const char *repo, struct svnae_txn *txn,
                      const char *author, const char *logmsg)
{
    return svnae_commit_finalise_with_props(repo, txn, author, logmsg, "");
}

/* Build a per-path props blob ("key=value\n" lines), write it to the
 * rep store, return its sha1 (as static thread-local, copy before
 * reuse). NULL on failure. */
const char *
svnae_build_props_blob(const char *repo,
                      const char *const *keys,
                      const char *const *values,
                      int n_pairs)
{
    /* Sort (key, value) pairs by key so identical prop sets always
     * hash to the same sha1. Cheap: bubble sort on indices. */
    int *order = malloc(sizeof(int) * (size_t)(n_pairs > 0 ? n_pairs : 1));
    for (int i = 0; i < n_pairs; i++) order[i] = i;
    for (int i = 0; i < n_pairs; i++) {
        for (int j = i + 1; j < n_pairs; j++) {
            if (strcmp(keys[order[i]], keys[order[j]]) > 0) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    body[0] = '\0';
    for (int i = 0; i < n_pairs; i++) {
        int idx = order[i];
        size_t need = strlen(keys[idx]) + 1 + strlen(values[idx]) + 1 + 1;
        if (len + need >= cap) {
            cap = (len + need) * 2;
            body = realloc(body, cap);
        }
        len += (size_t)snprintf(body + len, cap - len, "%s=%s\n", keys[idx], values[idx]);
    }
    free(order);
    const char *sha = svnae_rep_write_blob(repo, body, (int)len);
    free(body);
    return sha;   /* static-thread-local in rep_store_shim */
}

/* Build a per-path ACL blob (one "+user" or "-user" per line, caller
 * supplies sorted rules). Write to rep store, return sha. The blob
 * is content-addressed, so identical ACLs across paths share storage
 * just like anything else in the rep store. */
const char *
svnae_build_acl_blob(const char *repo,
                    const char *const *rules,
                    int n_rules)
{
    size_t cap = 128, len = 0;
    char *body = malloc(cap);
    body[0] = '\0';
    for (int i = 0; i < n_rules; i++) {
        size_t need = strlen(rules[i]) + 2;
        if (len + need >= cap) {
            cap = (len + need) * 2;
            body = realloc(body, cap);
        }
        len += (size_t)snprintf(body + len, cap - len, "%s\n", rules[i]);
    }
    const char *sha = svnae_rep_write_blob(repo, body, (int)len);
    free(body);
    return sha;
}

/* Build a paths-acl blob. The per-line "<acl-sha> <path>" lines are
 * assembled here, then sorted by path via aether_paths_index_sort_by_path
 * (ae/fs_fs/format_line.ae). Same shape as the paths-props blob. */
static const char *
build_paths_index_blob(const char *repo,
                       const char *const *paths,
                       const char *const *shas,
                       int n_paths)
{
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    body[0] = '\0';
    for (int i = 0; i < n_paths; i++) {
        size_t need = strlen(shas[i]) + 1 + strlen(paths[i]) + 1 + 1;
        if (len + need >= cap) {
            cap = (len + need) * 2;
            body = realloc(body, cap);
        }
        len += (size_t)snprintf(body + len, cap - len, "%s %s\n",
                                shas[i], paths[i]);
    }
    const char *sorted = aether_paths_index_sort_by_path(body);
    free(body);
    const char *sha = svnae_rep_write_blob(repo, sorted, (int)strlen(sorted));
    return sha;
}

const char *
svnae_build_paths_acl_blob(const char *repo,
                          const char *const *paths,
                          const char *const *acl_shas,
                          int n_paths)
{
    return build_paths_index_blob(repo, paths, acl_shas, n_paths);
}

const char *
svnae_build_paths_props_blob(const char *repo,
                            const char *const *paths,
                            const char *const *props_shas,
                            int n_paths)
{
    return build_paths_index_blob(repo, paths, props_shas, n_paths);
}

int
svnae_commit_finalise_with_props(const char *repo, struct svnae_txn *txn,
                                 const char *author, const char *logmsg,
                                 const char *props_sha1)
{
    return svnae_commit_finalise_with_acl(repo, txn, author, logmsg,
                                          props_sha1, "");
}

/* Finalise variant that also sets the paths-acl blob sha for this
 * rev. Empty string means "inherit from previous rev" (same rule as
 * props). Phase 7.1: ACL blob pointers live alongside props blobs
 * under a new `acl:` line in the rev blob. Revs with no acl field
 * default to inheriting; the first rev with no ancestor treats it
 * as empty (open). */
int
svnae_commit_finalise_with_acl(const char *repo, struct svnae_txn *txn,
                               const char *author, const char *logmsg,
                               const char *props_sha1, const char *acl_sha1)
{
    return svnae_commit_finalise_on_branch(repo, txn, "main", author, logmsg,
                                           props_sha1, acl_sha1);
}

/* Phase 8.1 branch-aware entry point. For every commit:
 *   - rev blob gets a `branch:` field identifying the branch advanced.
 *   - legacy $repo/revs/NNNNNN and $repo/head pointers are still
 *     written (simplifies incremental rollout; readers that predate
 *     branch support keep working).
 *   - new per-branch layout written too:
 *       $repo/branches/<name>/revs/aa/bb/NNNNNN
 *       $repo/branches/<name>/head  ("rev=N\n")
 *   - path_rev index gets one row per touched path in this rev.
 *
 * For Phase 8.1, `branch` is ignored beyond appearing in the rev
 * blob and selecting the per-branch paths — only "main" exists.
 * Phase 8.2 will add multi-branch create/switch. */
int
svnae_commit_finalise_on_branch(const char *repo, struct svnae_txn *txn,
                               const char *branch,
                               const char *author, const char *logmsg,
                               const char *props_sha1, const char *acl_sha1)
{
    if (!branch || !*branch) branch = "main";

    int base_rev = svnae_txn_base_rev(txn);
    if (base_rev < 0) return -1;
    char *base_root = load_rev_root_sha1(repo, base_rev);
    if (!base_root) return -1;

    char *new_root = svnae_txn_rebuild_root(repo, base_root, txn);
    free(base_root);
    if (!new_root) return -1;

    int prev = svnae_repos_head_rev(repo);
    if (prev < 0) { free(new_root); return -1; }
    int next = prev + 1;

    /* If caller didn't supply a props_sha1, inherit the previous rev's. */
    char *inherited_props = NULL;
    const char *ps = props_sha1 && *props_sha1 ? props_sha1 : NULL;
    if (!ps) {
        inherited_props = rev_blob_field(repo, prev, "props");
        ps = inherited_props ? inherited_props : "";
    }

    /* Same for ACL — inherit if caller didn't explicitly set. */
    char *inherited_acl = NULL;
    const char *as = acl_sha1 && *acl_sha1 ? acl_sha1 : NULL;
    if (!as) {
        inherited_acl = rev_blob_field(repo, prev, "acl");
        as = inherited_acl ? inherited_acl : "";
    }

    /* Build revision blob — rev_blob_body lives in ae/fs_fs/format_line.ae. */
    const char *rev_body = aether_rev_blob_body(new_root, branch, ps, as,
                                                prev, author,
                                                svnae_fsfs_now_iso8601(), logmsg);
    free(inherited_props);
    free(inherited_acl);

    const char *rev_sha = svnae_rep_write_blob(repo, rev_body, (int)strlen(rev_body));
    if (!rev_sha) { free(new_root); return -1; }
    /* Copy out of the thread-local buffer before it's reused for
     * subsequent blob writes (path_rev secondary-hash inserts may
     * call svnae_rep_write_blob again downstream). */
    char rev_sha_copy[65];
    {
        size_t rl = strlen(rev_sha);
        if (rl >= sizeof rev_sha_copy) { free(new_root); return -1; }
        memcpy(rev_sha_copy, rev_sha, rl + 1);
    }

    /* Legacy pointer. */
    char ptr_path[PATH_MAX];
    snprintf(ptr_path, sizeof ptr_path, "%s/revs/%06d", repo, next);
    char ptr_body[128];
    int plen = snprintf(ptr_body, sizeof ptr_body, "%s\n", rev_sha_copy);
    if (write_atomic(ptr_path, ptr_body, plen) != 0) { free(new_root); return -1; }

    /* Per-branch pointer with two-level fanout: aa/bb/NNNNNN where
     * aa = hex(rev >> 16), bb = hex((rev >> 8) & 0xff). At rev 10M
     * this gives ~150 files per leaf dir. */
    int aa = (next >> 16) & 0xff;
    int bb = (next >> 8) & 0xff;
    char branch_dir[PATH_MAX];
    snprintf(branch_dir, sizeof branch_dir, "%s/branches/%s/revs/%02x/%02x",
             repo, branch, aa, bb);
    /* mkdir -p. write_atomic writes to a tempfile + rename, so the
     * parent dir must exist first. */
    if (aether_io_mkdir_p(branch_dir) != 0) { free(new_root); return -1; }
    char branch_ptr[PATH_MAX];
    snprintf(branch_ptr, sizeof branch_ptr, "%s/%06d", branch_dir, next);
    if (write_atomic(branch_ptr, ptr_body, plen) != 0) { free(new_root); return -1; }

    /* Legacy head. */
    char head_path[PATH_MAX];
    snprintf(head_path, sizeof head_path, "%s/head", repo);
    char head_body[32];
    int hlen = snprintf(head_body, sizeof head_body, "%d\n", next);
    if (write_atomic(head_path, head_body, hlen) != 0) { free(new_root); return -1; }

    /* Per-branch head. */
    char br_head_path[PATH_MAX];
    snprintf(br_head_path, sizeof br_head_path, "%s/branches/%s/head", repo, branch);
    char br_head_body[64];
    int bhl = snprintf(br_head_body, sizeof br_head_body, "rev=%d\n", next);
    if (write_atomic(br_head_path, br_head_body, bhl) != 0) { free(new_root); return -1; }

    /* Populate path_rev index: one row per path this rev touched.
     * We derive the set from a tree-diff against base_rev's root,
     * reusing svnae_repos_paths_changed. Falls through silently if
     * the table doesn't exist yet (old repos), so this is safe
     * during the gradual migration. */
    {
        extern struct svnae_paths *svnae_repos_paths_changed(const char *repo, int rev);
        extern int                 svnae_repos_paths_count(const struct svnae_paths *P);
        extern const char         *svnae_repos_paths_path(const struct svnae_paths *P, int i);
        extern void                svnae_repos_paths_free(struct svnae_paths *P);

        /* We just wrote the rev's pointer file, so paths_changed(next)
         * will diff new_root against prev's root. */
        struct svnae_paths *P = svnae_repos_paths_changed(repo, next);
        if (P) {
            int pn = svnae_repos_paths_count(P);
            if (pn > 0) {
                char cache_path[PATH_MAX];
                snprintf(cache_path, sizeof cache_path, "%s/rep-cache.db", repo);
                sqlite3 *db = NULL;
                if (sqlite3_open_v2(cache_path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
                    /* Create the index table on demand. Seeded-from-scratch
                     * repos (via seed.ae) don't go through svnadmin/create,
                     * so their rep-cache.db won't have this table yet. */
                    sqlite3_exec(db,
                        "CREATE TABLE IF NOT EXISTS path_rev ("
                        "  branch TEXT NOT NULL,"
                        "  path   TEXT NOT NULL,"
                        "  rev    INTEGER NOT NULL,"
                        "  PRIMARY KEY (branch, path, rev));"
                        "CREATE INDEX IF NOT EXISTS path_rev_lookup "
                        "  ON path_rev (branch, path);",
                        NULL, NULL, NULL);
                    sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
                    sqlite3_stmt *st = NULL;
                    if (sqlite3_prepare_v2(db,
                            "INSERT OR IGNORE INTO path_rev (branch, path, rev) VALUES (?, ?, ?)",
                            -1, &st, NULL) == SQLITE_OK) {
                        for (int i = 0; i < pn; i++) {
                            sqlite3_bind_text(st, 1, branch, -1, SQLITE_TRANSIENT);
                            sqlite3_bind_text(st, 2, svnae_repos_paths_path(P, i),
                                              -1, SQLITE_TRANSIENT);
                            sqlite3_bind_int (st, 3, next);
                            sqlite3_step(st);
                            sqlite3_reset(st);
                        }
                        sqlite3_finalize(st);
                    }
                    sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
                    sqlite3_close(db);
                }
            }
            svnae_repos_paths_free(P);
        }
    }

    free(new_root);
    return next;
}

/* ---- branch create (Phase 8.2a) --------------------------------------
 *
 * Build a filtered copy of an existing branch's tree, using a set of
 * glob patterns to decide which paths survive. Writes new dir blobs
 * (content-addressed, so unchanged subtrees share with the source
 * branch), creates the branch head and initial rev pointer.
 *
 * Spec grammar: one glob per line. Matches with fnmatch(3), FNM_PATHNAME.
 * Examples:
 *   src/main.c       exact file
 *   README.md        exact file at root
 *   src/*            top-level files under src, not nested
 *   src/**           entire src subtree (we canonicalise "**" via a
 *                    prefix test, since fnmatch doesn't do ** natively)
 *
 * Returns the new rev number, or -1 on failure.
 */

#include <fnmatch.h>

/* FFI helper the Aether spec matcher (ae/fs_fs/spec.ae) calls. Keeps
 * fnmatch(3) on the C side since Aether has no binding for it. */
int32_t
svnae_fnmatch_pathname(const char *glob, const char *path)
{
    return fnmatch(glob, path, FNM_PATHNAME) == 0 ? 1 : 0;
}

/* Glob matcher ported to Aether (ae/fs_fs/spec.ae, --emit=lib). The
 * C wrapper just loops over globs[] and delegates each one to the
 * Aether per-glob matcher. The Aether side implements "prefix/**"
 * subtree inclusion, fnmatch via the FFI helper above, and the
 * ancestor-of-literal-glob rule. */
extern int32_t aether_spec_path_matches_glob(const char *path, const char *glob);

static int
path_matches_any(const char *path, const char *const *globs, int n)
{
    for (int i = 0; i < n; i++) {
        if (aether_spec_path_matches_glob(path, globs[i])) return 1;
    }
    return 0;
}

/* Recursive dir filter. Reads the dir blob at `src_sha` (a dir in the
 * source tree), emits a filtered dir blob, returns its new sha.
 *
 * `prefix` is the repo-relative path of this dir ("" for root).
 *
 * Returns malloc'd hex sha, or NULL on failure. On "no children
 * survived" returns the sha of the empty-dir blob (""). */
extern int         aether_dir_count_entries(const char *body);
extern int         aether_dir_entry_kind(const char *body, int i);
extern const char *aether_dir_entry_sha(const char *body, int i);
extern const char *aether_dir_entry_name(const char *body, int i);

static char *
filter_dir_recursive(const char *repo, const char *src_sha,
                     const char *prefix,
                     const char *const *globs, int n_globs)
{
    char *src_body = svnae_rep_read_blob(repo, src_sha);
    if (!src_body) return NULL;

    /* Build the filtered dir body line by line. */
    size_t cap = 256, blen = 0;
    char *body = malloc(cap);
    body[0] = '\0';

    int n_entries = aether_dir_count_entries(src_body);
    for (int ei = 0; ei < n_entries; ei++) {
        char kind_c = (char)aether_dir_entry_kind(src_body, ei);
        /* Snapshot child_sha / child_name — aether_* return pointers
         * into thread-local buffers that any later aether_* call (e.g.
         * the recursive one below) will overwrite. */
        char child_sha[65], child_name[PATH_MAX];
        const char *sha_ref = aether_dir_entry_sha(src_body, ei);
        size_t sha_len = strlen(sha_ref);
        if (sha_len >= sizeof child_sha) continue;
        memcpy(child_sha, sha_ref, sha_len + 1);
        const char *name_ref = aether_dir_entry_name(src_body, ei);
        size_t name_len = strlen(name_ref);
        if (name_len >= sizeof child_name) continue;
        memcpy(child_name, name_ref, name_len + 1);

        extern const char *aether_path_join_rel(const char *prefix, const char *name);
        const char *child_path = aether_path_join_rel(prefix, child_name);

        int include = 0;
        char emit_sha[65];
        emit_sha[0] = '\0';

        if (kind_c == 'f') {
            if (path_matches_any(child_path, globs, n_globs)) {
                include = 1;
                memcpy(emit_sha, child_sha, sha_len + 1);
            }
        } else if (kind_c == 'd') {
            /* Recurse. Include the dir if (a) the dir itself matches
             * or (b) any descendant does. filter_dir_recursive returns
             * the empty-dir sha if nothing survived — detect that and
             * skip the dir. */
            char *sub = filter_dir_recursive(repo, child_sha,
                                             child_path,
                                             globs, n_globs);
            if (sub) {
                char *empty = svnae_rep_read_blob(repo, sub);
                int sub_is_empty = (empty && *empty == '\0');
                if (empty) svnae_rep_free(empty);
                if (!sub_is_empty
                    || path_matches_any(child_path, globs, n_globs)) {
                    include = 1;
                    size_t sl = strlen(sub);
                    if (sl < 65) {
                        memcpy(emit_sha, sub, sl + 1);
                    }
                }
                free(sub);
            }
        }

        if (include) {
            extern const char *aether_dir_entry_line(int kind, const char *sha, const char *name);
            const char *line = aether_dir_entry_line((int)kind_c, emit_sha, child_name);
            size_t ln = strlen(line);
            if (blen + ln + 1 >= cap) {
                cap = (blen + ln + 1) * 2;
                body = realloc(body, cap);
            }
            memcpy(body + blen, line, ln);
            blen += ln;
            body[blen] = '\0';
        }
    }
    svnae_rep_free(src_body);

    const char *new_sha = svnae_rep_write_blob(repo, body, (int)blen);
    free(body);
    if (!new_sha) return NULL;
    return strdup(new_sha);
}

/* Create a new branch `name` from branch `base` at its current head,
 * filtered through `globs`. Writes:
 *   $repo/branches/<name>/spec         — glob per line
 *   $repo/branches/<name>/head         — rev=N
 *   $repo/branches/<name>/revs/00/00/NNNNNN — rev pointer
 *
 * Also writes a new rev blob with `branch: <name>` and the filtered
 * root sha. Returns the new rev number on success, -1 on failure.
 *
 * Constraints:
 *   - `name` must not already exist as a branch.
 *   - `base` must exist.
 *   - `globs` must be non-empty.
 */
int
svnae_branch_create(const char *repo, const char *name, const char *base,
                    const char *const *globs, int n_globs,
                    const char *author)
{
    if (!name || !*name || !base || !*base || n_globs <= 0) return -1;

    /* Reject if branch already exists. */
    extern int aether_io_exists(const char *path);
    char br_dir[PATH_MAX];
    snprintf(br_dir, sizeof br_dir, "%s/branches/%s", repo, name);
    if (aether_io_exists(br_dir)) return -1;

    /* Find base branch's head rev. Prefer the new per-branch head
     * file; fall back to the legacy $repo/head when base == "main"
     * and the per-branch file isn't there yet (e.g. for seeded repos
     * whose first commit went through the Phase-8.1 machinery but
     * whose seed didn't create the branches/main skeleton). */
    char base_head_path[PATH_MAX];
    snprintf(base_head_path, sizeof base_head_path, "%s/branches/%s/head", repo, base);
    int base_rev = -1;
    if (aether_io_file_size(base_head_path) >= 0) {
        const char *head_line = aether_io_read_file(base_head_path);
        const char *eq = strchr(head_line, '=');
        if (eq) base_rev = atoi(eq + 1);
    }
    if (base_rev < 0 && strcmp(base, "main") == 0) {
        /* Legacy repos (seeded before Phase 8.1) have no $repo/branches/main/
         * skeleton. We *must* read main's head from $repo/head here, but only
         * if no other branches exist yet — once another branch exists, the
         * legacy head file is the max rev across all branches, not main's
         * head specifically. Auto-materialize branches/main/head on success
         * so subsequent branch-creates find the per-branch file. */
        extern void *aether_io_listdir(const char *path);
        extern int aether_io_listdir_count(void *h);
        extern const char *aether_io_listdir_name(void *h, int i);
        extern void aether_io_listdir_free(void *h);

        char branches_root[PATH_MAX];
        snprintf(branches_root, sizeof branches_root, "%s/branches", repo);
        void *bd = aether_io_listdir(branches_root);
        int other_branches = 0;
        if (bd) {
            int n_br = aether_io_listdir_count(bd);
            for (int i = 0; i < n_br; i++) {
                const char *nm = aether_io_listdir_name(bd, i);
                if (strcmp(nm, "main") == 0) continue;
                other_branches = 1;
                break;
            }
            aether_io_listdir_free(bd);
        }
        if (!other_branches) {
            char legacy[PATH_MAX];
            snprintf(legacy, sizeof legacy, "%s/head", repo);
            if (aether_io_file_size(legacy) >= 0) {
                base_rev = atoi(aether_io_read_file(legacy));
            }
            if (base_rev >= 0) {
                /* Materialize $repo/branches/main/head so future lookups
                 * don't hit this fallback after another branch bumps the
                 * legacy head. */
                char main_dir[PATH_MAX];
                snprintf(main_dir, sizeof main_dir, "%s/branches/main", repo);
                (void)aether_io_mkdir_p(main_dir);
                char main_head[PATH_MAX];
                snprintf(main_head, sizeof main_head, "%s/head", main_dir);
                char hb[32];
                int hl = snprintf(hb, sizeof hb, "rev=%d\n", base_rev);
                (void)write_atomic(main_head, hb, hl);
            }
        }
    }
    if (base_rev < 0) return -1;

    char *base_root = load_rev_root_sha1(repo, base_rev);
    if (!base_root) return -1;

    /* Filter base's tree. */
    char *new_root = filter_dir_recursive(repo, base_root, "", globs, n_globs);
    free(base_root);
    if (!new_root) return -1;

    /* Write the spec blob (newline-separated globs) on disk under
     * $repo/branches/<name>/spec. */
    /* Parent $repo/branches may not exist yet either (for repos seeded
     * before Phase 8.1 landed). */
    if (aether_io_mkdir_p(br_dir) != 0) { free(new_root); return -1; }
    char spec_path[PATH_MAX];
    snprintf(spec_path, sizeof spec_path, "%s/spec", br_dir);
    {
        size_t cap = 128, slen = 0;
        char *spec_body = malloc(cap);
        spec_body[0] = '\0';
        for (int i = 0; i < n_globs; i++) {
            size_t need = strlen(globs[i]) + 2;
            if (slen + need >= cap) {
                cap = (slen + need) * 2;
                spec_body = realloc(spec_body, cap);
            }
            slen += snprintf(spec_body + slen, cap - slen, "%s\n", globs[i]);
        }
        if (write_atomic(spec_path, spec_body, slen) != 0) {
            free(spec_body); free(new_root); return -1;
        }
        free(spec_body);
    }

    /* Create the revs directory structure. */
    char revs_dir[PATH_MAX];
    snprintf(revs_dir, sizeof revs_dir, "%s/revs/00/00", br_dir);
    if (aether_io_mkdir_p(revs_dir) != 0) { free(new_root); return -1; }

    /* New rev number: one past the repo's current max.
     *
     * Cross-branch rev numbering is a deliberate design choice: rev
     * numbers are per-repo, not per-branch. This is simpler to reason
     * about (every rev has a unique int id) and matches Perforce's
     * changelist numbering. Alternative: per-branch rev sequences —
     * slightly cleaner but makes cross-branch merges ambiguous about
     * "what rev number am I at". */
    int new_rev = svnae_repos_head_rev(repo) + 1;

    /* Write rev blob. */
    const char *prev_props = "";
    const char *prev_acl   = "";

    char logmsg[256];
    snprintf(logmsg, sizeof logmsg, "create branch %s from %s with %d include(s)",
             name, base, n_globs);

    const char *rev_body = aether_rev_blob_body(new_root, name,
                                                 prev_props, prev_acl,
                                                 base_rev, author,
                                                 svnae_fsfs_now_iso8601(), logmsg);
    const char *rev_sha = svnae_rep_write_blob(repo, rev_body, (int)strlen(rev_body));
    if (!rev_sha) { free(new_root); return -1; }
    char rev_sha_copy[65];
    {
        size_t rl = strlen(rev_sha);
        if (rl >= sizeof rev_sha_copy) { free(new_root); return -1; }
        memcpy(rev_sha_copy, rev_sha, rl + 1);
    }

    /* Rev pointer. */
    char ptr_body[128];
    int plen = snprintf(ptr_body, sizeof ptr_body, "%s\n", rev_sha_copy);
    char ptr_path[PATH_MAX];
    snprintf(ptr_path, sizeof ptr_path, "%s/%06d", revs_dir, new_rev);
    if (write_atomic(ptr_path, ptr_body, plen) != 0) { free(new_root); return -1; }

    /* Also write the legacy $repo/revs/NNNNNN pointer so the single-
     * branch readers (e.g. the current /info and /log endpoints) can
     * still traverse the rev space. */
    char legacy_ptr[PATH_MAX];
    snprintf(legacy_ptr, sizeof legacy_ptr, "%s/revs/%06d", repo, new_rev);
    if (write_atomic(legacy_ptr, ptr_body, plen) != 0) { free(new_root); return -1; }

    /* Branch head. */
    char head_path[PATH_MAX];
    snprintf(head_path, sizeof head_path, "%s/head", br_dir);
    char head_body[64];
    int hlen = snprintf(head_body, sizeof head_body, "rev=%d\n", new_rev);
    if (write_atomic(head_path, head_body, hlen) != 0) { free(new_root); return -1; }

    /* Legacy head file moves to max rev of any branch. */
    char legacy_head[PATH_MAX];
    snprintf(legacy_head, sizeof legacy_head, "%s/head", repo);
    char lh[32];
    int lhlen = snprintf(lh, sizeof lh, "%d\n", new_rev);
    write_atomic(legacy_head, lh, lhlen);

    free(new_root);
    return new_rev;
}

/* The glob-parsing + matching is ported to Aether in ae/fs_fs/spec.ae.
 * The C side keeps only the file I/O (std.fs is unavailable under
 * --emit=lib) and hands the spec-file body to the Aether entry point. */
extern int32_t aether_spec_matches_any(const char *path, const char *specs_joined);

/* Phase 8.2b: is `path` allowed by `branch`'s include spec?
 *   - Empty or missing spec ⇒ allow (main's "full tree" case).
 *   - Otherwise, use the same matcher as tree filtering at creation time.
 *
 * A branch's head rev number is fine for the spec lookup: specs are
 * keyed by branch name on disk, not by rev. Specs aren't versioned yet
 * (Phase 8.2; evolving specs across revs is a later concern).
 *
 * Returns 1 = allowed, 0 = denied, -1 on I/O error (caller treats as
 * deny to be safe).
 */
int
svnae_branch_spec_allows(const char *repo, const char *branch, const char *path)
{
    if (!repo || !branch || !path) return -1;
    if (!*branch || !*path) return 1;

    char spec_path[PATH_MAX];
    snprintf(spec_path, sizeof spec_path, "%s/branches/%s/spec", repo, branch);
    int sz = aether_io_file_size(spec_path);
    if (sz < 0) {
        /* No spec file → behave like main: include-all. Applies to
         * legacy seeded repos where $repo/branches/main/spec doesn't
         * exist at all. */
        return 1;
    }
    if (sz == 0) return 1;   /* empty spec → include-all */

    const char *buf = aether_io_read_file(spec_path);
    int ok = aether_spec_matches_any(path, buf);
    return ok ? 1 : 0;
}
