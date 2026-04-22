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

/* ae/wc/commit_shim.c — wc-backed commit.
 *
 * Packages the WC's pending changes into an RA commit:
 *   1. Walk wc.db for rows with state != 0 (added, deleted, replaced).
 *      Also walk state=0 file rows and compare their on-disk sha1
 *      against base_sha1 → if different, treat as modified (→ add-file
 *      edit in the txn).
 *   2. Read info kv to get base_url, repo_name, base_rev.
 *   3. Build an RA commit via the Phase 7 API, POST it.
 *   4. On success, rewrite node rows: state=0, base_rev=new_rev,
 *      base_sha1=current content sha1 for added/modified, delete rows
 *      for deleted.
 *   5. Also refresh the pristine store with the newly-committed bytes.
 *
 * API:
 *   svnae_wc_commit(wc_root, author, logmsg) -> new_rev | -1
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

/* --- externs --------------------------------------------------------- */

sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int      svnae_wc_db_delete_node(sqlite3 *db, const char *path);

struct svnae_wc_nodelist;
struct svnae_wc_nodelist *svnae_wc_db_list_nodes(sqlite3 *db);
int         svnae_wc_nodelist_count(const struct svnae_wc_nodelist *L);
const char *svnae_wc_nodelist_path(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_kind(const struct svnae_wc_nodelist *L, int i);
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_state(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_conflicted(const struct svnae_wc_nodelist *L, int i);
void        svnae_wc_nodelist_free(struct svnae_wc_nodelist *L);

char *svnae_wc_db_get_info(sqlite3 *db, const char *key);
int   svnae_wc_db_set_info(sqlite3 *db, const char *key, const char *value);
void  svnae_wc_info_free(char *s);

const char *svnae_wc_pristine_put(const char *wc_root, const char *data, int len);

struct svnae_ra_commit;
struct svnae_ra_commit *svnae_ra_commit_begin(int base_rev, const char *author, const char *logmsg);
int  svnae_ra_commit_add_file(struct svnae_ra_commit *cb, const char *path, const char *content, int len);
int  svnae_ra_commit_mkdir(struct svnae_ra_commit *cb, const char *path);
int  svnae_ra_commit_delete(struct svnae_ra_commit *cb, const char *path);
int  svnae_ra_commit_set_prop(struct svnae_ra_commit *cb, const char *path, const char *key, const char *value);
int  svnae_ra_commit_finish(struct svnae_ra_commit *cb, const char *base_url, const char *repo_name);

struct svnae_wc_proplist;
struct svnae_wc_proplist *svnae_wc_proplist(const char *wc_root, const char *path);
int          svnae_wc_proplist_count(const struct svnae_wc_proplist *L);
const char  *svnae_wc_proplist_name (const struct svnae_wc_proplist *L, int i);
const char  *svnae_wc_proplist_value(const struct svnae_wc_proplist *L, int i);
void         svnae_wc_proplist_free (struct svnae_wc_proplist *L);

/* --- helpers --------------------------------------------------------- */

extern int svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out);
extern int svnae_wc_hash_file (const char *wc_root, const char *path, char *out);

static __thread const char *g_wc_root = NULL;

static int
sha1_of_file(const char *path, char out[65])
{
    if (!g_wc_root) return -1;
    return svnae_wc_hash_file(g_wc_root, path, out);
}

static char *
read_file_to_malloc(const char *path, int *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t got = 0;
    while (got < st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)(st.st_size - got));
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (n == 0) break;
        got += n;
    }
    close(fd);
    buf[got] = '\0';
    *out_len = (int)got;
    return buf;
}

/* --- main entry point ------------------------------------------------ */

int
svnae_wc_commit(const char *wc_root, const char *author, const char *logmsg)
{
    g_wc_root = wc_root;
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;

    char *base_url = svnae_wc_db_get_info(db, "base_url");
    char *repo     = svnae_wc_db_get_info(db, "repo");
    char *base_rev_s = svnae_wc_db_get_info(db, "base_rev");
    if (!base_url || !repo || !base_rev_s) {
        svnae_wc_db_close(db);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo); svnae_wc_info_free(base_rev_s);
        return -1;
    }
    int base_rev = atoi(base_rev_s);
    svnae_wc_info_free(base_rev_s);

    /* Gather pending edits from the WC. We collect into parallel arrays
     * so we can redo the db writes after a successful commit. */
    struct svnae_wc_nodelist *L = svnae_wc_db_list_nodes(db);
    int n = svnae_wc_nodelist_count(L);

    /* Refuse to commit if any node is conflicted. The user must resolve
     * first. Match reference svn's behaviour. */
    for (int i = 0; i < n; i++) {
        if (svnae_wc_nodelist_conflicted(L, i)) {
            fprintf(stderr,
                "svn: commit failed: '%s' remains in conflict; run svn resolve first\n",
                svnae_wc_nodelist_path(L, i));
            svnae_wc_nodelist_free(L);
            svnae_wc_db_close(db);
            svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
            return -3;   /* -3 = conflicted paths present */
        }
    }

    /* Two-pass: first build the RA commit, then if it succeeds, walk the
     * same list again and update rows. We keep the rows' roles in a
     * small per-node int array:
     *   0 = no edit
     *   1 = add-file (new file or modified)
     *   2 = mkdir
     *   3 = delete
     */
    size_t n_alloc = n > 0 ? (size_t)n : 1;
    int *roles = calloc(n_alloc, sizeof *roles);
    char **new_sha1s = calloc(n_alloc, sizeof *new_sha1s);   /* for role=1 files */

    struct svnae_ra_commit *cb = svnae_ra_commit_begin(base_rev, author, logmsg);
    int any_edits = 0;

    for (int i = 0; i < n; i++) {
        const char *rel   = svnae_wc_nodelist_path(L, i);
        int kind          = svnae_wc_nodelist_kind(L, i);
        int state         = svnae_wc_nodelist_state(L, i);
        const char *base_sha = svnae_wc_nodelist_base_sha1(L, i);

        char disk[PATH_MAX];
        snprintf(disk, sizeof disk, "%s/%s", wc_root, rel);

        if (state == 2 /*deleted*/) {
            svnae_ra_commit_delete(cb, rel);
            roles[i] = 3;
            any_edits = 1;
            continue;
        }
        if (state == 1 /*added*/) {
            if (kind == 1 /*dir*/) {
                svnae_ra_commit_mkdir(cb, rel);
                roles[i] = 2;
                any_edits = 1;
                continue;
            }
            /* added file — read content. */
            int len = 0;
            char *data = read_file_to_malloc(disk, &len);
            if (!data) continue;
            svnae_ra_commit_add_file(cb, rel, data, len);
            /* Record content-hash for post-commit pristine + db refresh
             * using the WC's configured algorithm (sha1 by default;
             * sha256 on Phase 6.1 repos). */
            char sha[65];
            if (svnae_wc_hash_bytes(wc_root, data, len, sha) == 0) {
                free(data); continue;
            }
            new_sha1s[i] = strdup(sha);
            svnae_wc_pristine_put(wc_root, data, len);
            free(data);
            roles[i] = 1;
            any_edits = 1;
            continue;
        }
        /* state == normal. For files: modified? */
        if (kind == 0 /*file*/) {
            char disk_sha[65];
            if (sha1_of_file(disk, disk_sha) != 0) continue;
            if (strcmp(disk_sha, base_sha) == 0) continue;  /* unchanged */
            /* Modified — same wire shape as add-file. */
            int len = 0;
            char *data = read_file_to_malloc(disk, &len);
            if (!data) continue;
            svnae_ra_commit_add_file(cb, rel, data, len);
            new_sha1s[i] = strdup(disk_sha);
            svnae_wc_pristine_put(wc_root, data, len);
            free(data);
            roles[i] = 1;
            any_edits = 1;
        }
    }

    /* Collect WC-side properties to send with the commit. Strategy:
     * for every node with at least one row in `props`, include ALL its
     * current props in the commit. Paths whose props haven't been
     * touched don't need to be re-sent — they inherit the previous
     * revision's props-sha1 via rep-sharing on the server. But since
     * Phase 5.14 doesn't yet track per-path dirty bits, we include
     * every tracked path with props. This is correct but noisier; an
     * optimisation pass can add delta tracking later. */
    for (int i = 0; i < n; i++) {
        const char *rel = svnae_wc_nodelist_path(L, i);
        struct svnae_wc_proplist *P = svnae_wc_proplist(wc_root, rel);
        if (!P) continue;
        int pn = svnae_wc_proplist_count(P);
        for (int j = 0; j < pn; j++) {
            svnae_ra_commit_set_prop(cb, rel,
                                     svnae_wc_proplist_name(P, j),
                                     svnae_wc_proplist_value(P, j));
        }
        svnae_wc_proplist_free(P);
    }

    if (!any_edits) {
        /* Nothing to commit. Clean up builder by calling finish anyway;
         * with an empty edit list the server still produces a new rev
         * (matching our HTTP-layer behaviour). For tests we want a
         * meaningful return value, so treat this as "no op" == -2. */
        svnae_ra_commit_finish(cb, base_url, repo);
        svnae_wc_nodelist_free(L);
        svnae_wc_db_close(db);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        for (int i = 0; i < n; i++) free(new_sha1s[i]);
        free(roles); free(new_sha1s);
        return -2;
    }

    int new_rev = svnae_ra_commit_finish(cb, base_url, repo);
    if (new_rev < 0) {
        svnae_wc_nodelist_free(L);
        svnae_wc_db_close(db);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        for (int i = 0; i < n; i++) free(new_sha1s[i]);
        free(roles); free(new_sha1s);
        return -1;
    }

    /* Success — reconcile wc.db with the new base revision. */
    for (int i = 0; i < n; i++) {
        const char *rel = svnae_wc_nodelist_path(L, i);
        int kind        = svnae_wc_nodelist_kind(L, i);
        int role = roles[i];
        if (role == 3) {
            svnae_wc_db_delete_node(db, rel);
        } else if (role == 2) {
            svnae_wc_db_upsert_node(db, rel, 1 /*dir*/, new_rev, "", 0);
        } else if (role == 1) {
            svnae_wc_db_upsert_node(db, rel, 0 /*file*/, new_rev, new_sha1s[i], 0);
        } else {
            /* Unchanged node — still bump base_rev to the new rev so
             * subsequent commits have a consistent base. */
            svnae_wc_db_upsert_node(db, rel, kind, new_rev,
                                    svnae_wc_nodelist_base_sha1(L, i), 0);
        }
    }

    /* Update base_rev in info. */
    char buf[16]; snprintf(buf, sizeof buf, "%d", new_rev);
    svnae_wc_db_set_info(db, "base_rev", buf);

    svnae_wc_nodelist_free(L);
    svnae_wc_db_close(db);
    svnae_wc_info_free(base_url);
    svnae_wc_info_free(repo);
    for (int i = 0; i < n; i++) free(new_sha1s[i]);
    free(roles); free(new_sha1s);
    return new_rev;
}
