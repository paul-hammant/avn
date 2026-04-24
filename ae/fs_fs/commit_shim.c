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
const char *svnae_fsfs_now_iso8601(void);
int         svnae_repos_head_rev(const char *repo);

/* Atomic write + mkdir-p ported to Aether (ae/subr/io.ae, --emit=lib --with=fs). */
extern int aether_io_write_atomic(const char *path, const char *data, int length);
extern int aether_io_mkdir_p(const char *path);

static int write_atomic(const char *path, const char *data, int len)
{
    return aether_io_write_atomic(path, data, len) == 0 ? 0 : -1;
}

/* Read the root-dir sha1 for a given revision. Returns malloc'd
 * string or NULL. rev_blob_field below already goes through
 * ae/repos/rev_io.ae for the two-hop read; define its extern
 * first so we can point at it. */
extern const char *aether_io_read_file(const char *path);
extern int aether_io_file_size(const char *path);
extern const char *aether_repos_load_rev_blob_field(const char *repo, int rev,
                                                    const char *key);

static char *
load_rev_root_sha1(const char *repo, int rev)
{
    const char *v = aether_repos_load_rev_blob_field(repo, rev, "root");
    if (!v || !*v) return NULL;
    return strdup(v);
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

extern const char *aether_rev_blob_body(const char *root, const char *branch,
                                        const char *props, const char *acl,
                                        int prev, const char *author,
                                        const char *date, const char *log);

/* rev_blob_field: thin wrapper around the same Aether helper
 * load_rev_root_sha1 uses. "NULL on miss, malloc'd on hit"
 * matches the in-file callers. */
static char *
rev_blob_field(const char *repo, int rev, const char *key)
{
    const char *v = aether_repos_load_rev_blob_field(repo, rev, key);
    if (!v || !*v) return NULL;
    return strdup(v);
}

int
svnae_commit_finalise(const char *repo, struct svnae_txn *txn,
                      const char *author, const char *logmsg)
{
    return svnae_commit_finalise_with_props(repo, txn, author, logmsg, "");
}

/* svnae_build_props_blob / svnae_build_acl_blob /
 * svnae_build_paths_{acl,props}_blob ported to
 * ae/svnserver/blob_build.ae. Aether builds the blob body directly
 * (sorts the props pairs by key, reuses aether_paths_index_sort_by_path
 * for the paths-* variants) and calls svnae_rep_write_blob itself;
 * the C round-trip through arrays is gone. */

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

    /* Write the 4-file pointer set (legacy rev pointer, per-branch
     * rev pointer with aa/bb fanout, legacy head, per-branch head).
     * Ported to ae/fs_fs/branch_spec.ae::write_rev_pointer_set. */
    extern int aether_write_rev_pointer_set(const char *repo, const char *branch,
                                             int next_rev, const char *rev_sha);
    if (aether_write_rev_pointer_set(repo, branch, next, rev_sha_copy) != 0) {
        free(new_root); return -1;
    }

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

/* Recursive dir filter ported to Aether (ae/fs_fs/filter.ae).
 * svnae_branch_create calls aether_filter_dir directly now. */

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
                    const char *globs_joined, const char *author)
{
    if (!name || !*name || !base || !*base || !globs_joined || !*globs_joined) return -1;

    /* Reject if branch already exists. */
    extern int aether_io_exists(const char *path);
    char br_dir[PATH_MAX];
    snprintf(br_dir, sizeof br_dir, "%s/branches/%s", repo, name);
    if (aether_io_exists(br_dir)) return -1;

    /* Find base branch's head rev. The Phase 8.1 new-layout probe,
     * legacy-main fallback, and auto-materialize of
     * $repo/branches/main/head are all ported to
     * ae/fs_fs/branch_spec.ae::branch_head_rev. */
    extern int aether_branch_head_rev(const char *repo, const char *base);
    int base_rev = aether_branch_head_rev(repo, base);
    if (base_rev < 0) return -1;

    char *base_root = load_rev_root_sha1(repo, base_rev);
    if (!base_root) return -1;

    /* Filter base's tree via the Aether recursive filter
     * (ae/fs_fs/filter.ae). Takes the globs already newline-joined —
     * same string we write to the branch's spec file below. */
    extern const char *aether_filter_dir(const char *repo, const char *src_sha,
                                         const char *prefix,
                                         const char *globs_joined);
    const char *new_root_ref = aether_filter_dir(repo, base_root, "", globs_joined);
    char *new_root = (new_root_ref && *new_root_ref) ? strdup(new_root_ref) : NULL;
    free(base_root);
    if (!new_root) return -1;

    /* Write the spec blob on disk under $repo/branches/<name>/spec.
     * Parent $repo/branches may not exist yet either (for repos seeded
     * before Phase 8.1 landed). */
    if (aether_io_mkdir_p(br_dir) != 0) { free(new_root); return -1; }
    char spec_path[PATH_MAX];
    snprintf(spec_path, sizeof spec_path, "%s/spec", br_dir);
    /* globs_joined is "glob1\nglob2\n..." — usable as spec-file content
     * directly. Ensure a trailing newline for the on-disk form. */
    {
        int glen = (int)strlen(globs_joined);
        int has_nl = glen > 0 && globs_joined[glen - 1] == '\n';
        if (has_nl) {
            if (write_atomic(spec_path, globs_joined, glen) != 0) {
                free(new_root); return -1;
            }
        } else {
            char *with_nl = malloc((size_t)glen + 2);
            if (!with_nl) { free(new_root); return -1; }
            memcpy(with_nl, globs_joined, (size_t)glen);
            with_nl[glen] = '\n';
            with_nl[glen + 1] = '\0';
            int wrc = write_atomic(spec_path, with_nl, glen + 1);
            free(with_nl);
            if (wrc != 0) { free(new_root); return -1; }
        }
    }

    /* The per-branch revs/aa/bb directory is created by
     * aether_write_rev_pointer_set below (it knows the fanout math).
     * The old inline "mkdir -p $br_dir/revs/00/00" was a latent bug:
     * fine for rev < 256 but wrong dir for rev >= 256. */

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

    /* Count includes by counting non-empty newline-separated segments
     * in globs_joined — we dropped the pre-split n_globs parameter. */
    int n_globs = 0;
    {
        const char *q = globs_joined;
        while (*q) {
            const char *nl = strchr(q, '\n');
            size_t seg = nl ? (size_t)(nl - q) : strlen(q);
            if (seg > 0) n_globs++;
            if (!nl) break;
            q = nl + 1;
        }
    }
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

    /* Write the 4-file pointer set. Shared with commit_finalise_on_branch
     * via ae/fs_fs/branch_spec.ae::write_rev_pointer_set. */
    if (aether_write_rev_pointer_set(repo, name, new_rev, rev_sha_copy) != 0) {
        free(new_root); return -1;
    }

    free(new_root);
    return new_rev;
}

/* The glob-parsing + matching is ported to Aether in ae/fs_fs/spec.ae.
 * The branch-spec logic (including empty-spec = allow) is ported to
 * ae/fs_fs/branch_spec.ae; aether_branch_spec_allows is called
 * directly where needed. */

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
/* Ported to ae/fs_fs/branch_spec.ae. Thin C shim preserves the
 * existing -1-on-bad-args shape the old C function had (Aether
 * version simplifies to 1 on bad branch/path, since no caller
 * differentiated). */
extern int aether_branch_spec_allows(const char *repo, const char *branch,
                                     const char *path);
int
svnae_branch_spec_allows(const char *repo, const char *branch, const char *path)
{
    if (!repo || !branch || !path) return -1;
    return aether_branch_spec_allows(repo, branch, path);
}
