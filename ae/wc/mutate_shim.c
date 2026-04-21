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

/* ae/wc/mutate_shim.c — WC-level add / rm.
 *
 * These are tiny: they only change wc.db state rows. The actual file
 * write (for add) is the user's responsibility — they create the file
 * first and then `svn add PATH` registers it. `svn rm PATH` removes
 * the file from disk AND marks the node deleted in the db.
 *
 * State transitions:
 *   add(path):
 *     - file must exist on disk, must not already be tracked
 *     - upsert nodes(path, file, 0, "", 1=added)
 *   rm(path):
 *     - if path is tracked, set state=deleted (keep for commit)
 *     - delete the file from disk
 *     - if path was only added (never committed), remove the row entirely
 *
 * Per-shim API:
 *   svnae_wc_add(wc_root, rel_path) -> 0 on success, -1 error
 *   svnae_wc_rm (wc_root, rel_path) -> 0 on success, -1 error
 */

#include <errno.h>
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
int      svnae_wc_db_node_exists(sqlite3 *db, const char *path);

struct svnae_wc_node;
struct svnae_wc_node *svnae_wc_db_get_node(sqlite3 *db, const char *path);
int         svnae_wc_node_kind    (const struct svnae_wc_node *n);
int         svnae_wc_node_state   (const struct svnae_wc_node *n);
int         svnae_wc_node_base_rev(const struct svnae_wc_node *n);
const char *svnae_wc_node_base_sha1(const struct svnae_wc_node *n);
void        svnae_wc_node_free    (struct svnae_wc_node *n);

/* Return 0 on success, -1 if path already tracked, -2 if not on disk,
 * -3 on db error. */
int
svnae_wc_add(const char *wc_root, const char *rel_path)
{
    char disk[PATH_MAX];
    snprintf(disk, sizeof disk, "%s/%s", wc_root, rel_path);
    struct stat st;
    if (stat(disk, &st) != 0) return -2;

    int kind = S_ISDIR(st.st_mode) ? 1 : 0;

    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -3;

    if (svnae_wc_db_node_exists(db, rel_path)) {
        /* Already tracked — but could be a previously-scheduled delete
         * that we're un-deleting. For Phase 5 we just reject; users can
         * call `svn rm` first if they want to re-add. */
        svnae_wc_db_close(db);
        return -1;
    }

    int rc = svnae_wc_db_upsert_node(db, rel_path, kind, 0, "", 1 /*added*/);
    svnae_wc_db_close(db);
    return rc == 0 ? 0 : -3;
}

/* Return 0 on success, -1 if not tracked, -2 on db error. Removes the
 * file from disk (best-effort) and marks node deleted. If the node was
 * state=added (never committed) we drop the row entirely instead. */
int
svnae_wc_rm(const char *wc_root, const char *rel_path)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -2;

    struct svnae_wc_node *n = svnae_wc_db_get_node(db, rel_path);
    if (!n) { svnae_wc_db_close(db); return -1; }

    int state = svnae_wc_node_state(n);
    int kind  = svnae_wc_node_kind(n);
    int base_rev = svnae_wc_node_base_rev(n);
    char base_sha[41] = {0};
    const char *bs = svnae_wc_node_base_sha1(n);
    strncpy(base_sha, bs ? bs : "", sizeof base_sha - 1);
    svnae_wc_node_free(n);

    if (state == 1 /*added*/) {
        /* Revert the add — just drop the row. Do NOT delete the on-disk
         * file; the user didn't intend to lose content they just added. */
        int rc = svnae_wc_db_delete_node(db, rel_path);
        svnae_wc_db_close(db);
        return rc == 0 ? 0 : -2;
    }

    /* Normal or already-deleted: mark deleted — but KEEP the base_rev
     * and base_sha1 so `svn revert` can restore from pristine later. */
    int rc = svnae_wc_db_upsert_node(db, rel_path, kind, base_rev, base_sha, 2 /*deleted*/);
    svnae_wc_db_close(db);
    if (rc != 0) return -2;

    /* Remove the file/dir from disk best-effort. Reference svn keeps
     * deleted files around until commit in some cases; we remove now
     * because this CLI is scoped for "commit soon". */
    char disk[PATH_MAX];
    snprintf(disk, sizeof disk, "%s/%s", wc_root, rel_path);
    if (kind == 0) unlink(disk);
    else           rmdir(disk);

    return 0;
}
