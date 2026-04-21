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

/* Atomic write + fsync — re-used here so we don't introduce a dep edge
 * from this shim back to the one that owns the util. */
static int
write_atomic(const char *path, const char *data, int len)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    const char *p = data;
    int rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -errno; }
        p += w; rem -= (int)w;
    }
    if (fsync(fd) != 0) { int rc = -errno; close(fd); unlink(tmp); return rc; }
    if (close(fd) != 0) return -errno;
    if (rename(tmp, path) != 0) { unlink(tmp); return -errno; }
    return 0;
}

/* Read the root-dir sha1 for a given revision. Returns malloc'd string
 * or NULL. */
static char *
load_rev_root_sha1(const char *repo, int rev)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/revs/%06d", repo, rev);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[128];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return NULL; }
    fclose(f);
    /* trim trailing whitespace */
    size_t n = strlen(buf);
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

/* Read a "key: <value>\n" line from an existing revision blob (or
 * return NULL if not present). Caller frees. */
static char *
rev_blob_field(const char *repo, int rev, const char *key)
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
    size_t klen = strlen(key);
    char needle[64];
    snprintf(needle, sizeof needle, "%s: ", key);
    char *p = strstr(body, needle);
    char *out = NULL;
    if (p) {
        p += klen + 2;
        char *eol = strchr(p, '\n');
        size_t L = eol ? (size_t)(eol - p) : strlen(p);
        out = malloc(L + 1);
        memcpy(out, p, L);
        out[L] = '\0';
    }
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

/* Build a paths-acl blob ("<acl-sha> <path>\n" per line, sorted by
 * path). Same wire shape as the paths-props blob. */
const char *
svnae_build_paths_acl_blob(const char *repo,
                          const char *const *paths,
                          const char *const *acl_shas,
                          int n_paths)
{
    int *order = malloc(sizeof(int) * (size_t)(n_paths > 0 ? n_paths : 1));
    for (int i = 0; i < n_paths; i++) order[i] = i;
    for (int i = 0; i < n_paths; i++) {
        for (int j = i + 1; j < n_paths; j++) {
            if (strcmp(paths[order[i]], paths[order[j]]) > 0) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    body[0] = '\0';
    for (int i = 0; i < n_paths; i++) {
        int idx = order[i];
        size_t need = 64 + 1 + strlen(paths[idx]) + 1 + 1;
        if (len + need >= cap) {
            cap = (len + need) * 2;
            body = realloc(body, cap);
        }
        len += (size_t)snprintf(body + len, cap - len, "%s %s\n",
                                acl_shas[idx], paths[idx]);
    }
    free(order);
    const char *sha = svnae_rep_write_blob(repo, body, (int)len);
    free(body);
    return sha;
}

/* Build a paths-props blob ("<props-sha1> <path>\n" per line, sorted
 * by path), write to rep store, return its sha1. */
const char *
svnae_build_paths_props_blob(const char *repo,
                            const char *const *paths,
                            const char *const *props_shas,
                            int n_paths)
{
    int *order = malloc(sizeof(int) * (size_t)(n_paths > 0 ? n_paths : 1));
    for (int i = 0; i < n_paths; i++) order[i] = i;
    for (int i = 0; i < n_paths; i++) {
        for (int j = i + 1; j < n_paths; j++) {
            if (strcmp(paths[order[i]], paths[order[j]]) > 0) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    body[0] = '\0';
    for (int i = 0; i < n_paths; i++) {
        int idx = order[i];
        size_t need = 40 + 1 + strlen(paths[idx]) + 1 + 1;
        if (len + need >= cap) {
            cap = (len + need) * 2;
            body = realloc(body, cap);
        }
        len += (size_t)snprintf(body + len, cap - len, "%s %s\n",
                                props_shas[idx], paths[idx]);
    }
    free(order);
    const char *sha = svnae_rep_write_blob(repo, body, (int)len);
    free(body);
    return sha;
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

    /* Build revision blob — now includes `branch:`. */
    size_t est = strlen(author) + strlen(logmsg) + strlen(branch) + 320;
    char *rev_body = malloc(est);
    int n = snprintf(rev_body, est,
                     "root: %s\nbranch: %s\nprops: %s\nacl: %s\nprev: %d\nauthor: %s\ndate: %s\nlog: %s\n",
                     new_root, branch, ps, as, prev, author,
                     svnae_fsfs_now_iso8601(), logmsg);
    if (n >= (int)est) {
        free(rev_body);
        rev_body = malloc((size_t)n + 1);
        snprintf(rev_body, (size_t)n + 1,
                 "root: %s\nbranch: %s\nprops: %s\nacl: %s\nprev: %d\nauthor: %s\ndate: %s\nlog: %s\n",
                 new_root, branch, ps, as, prev, author,
                 svnae_fsfs_now_iso8601(), logmsg);
    }
    free(inherited_props);
    free(inherited_acl);

    const char *rev_sha = svnae_rep_write_blob(repo, rev_body, (int)strlen(rev_body));
    free(rev_body);
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
    {
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", branch_dir);
        for (char *q = tmp + 1; *q; q++) {
            if (*q == '/') {
                *q = '\0';
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(new_root); return -1; }
                *q = '/';
            }
        }
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(new_root); return -1; }
    }
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
