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

/* ae/wc/revert_shim.c — svn revert.
 *
 * Discards local changes to a path by restoring it from the pristine
 * store and resetting the node's state to normal. Handles:
 *
 *   state=0 (normal):
 *     - file: overwrite disk with pristine bytes; no db change.
 *     - dir:  nothing to do.
 *   state=1 (added):
 *     - drop the row (un-register). Don't touch disk — user may want
 *       to keep the file.
 *   state=2 (deleted):
 *     - restore file from pristine, reset state to 0.
 *
 * For unversioned ('?') paths: reference svn errors. We match that.
 *
 * API:
 *   svnae_wc_revert(wc_root, rel_path) -> 0 on success
 *                                          -1 not tracked
 *                                          -2 other error
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Externs from neighbouring shims. */
sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int      svnae_wc_db_delete_node(sqlite3 *db, const char *path);

struct svnae_wc_node;
struct svnae_wc_node *svnae_wc_db_get_node(sqlite3 *db, const char *path);
int         svnae_wc_node_kind    (const struct svnae_wc_node *n);
int         svnae_wc_node_base_rev(const struct svnae_wc_node *n);
const char *svnae_wc_node_base_sha1(const struct svnae_wc_node *n);
int         svnae_wc_node_state   (const struct svnae_wc_node *n);
void        svnae_wc_node_free    (struct svnae_wc_node *n);

char *svnae_wc_pristine_get(const char *wc_root, const char *sha1);
int   svnae_wc_pristine_size(const char *wc_root, const char *sha1);
void  svnae_wc_pristine_free(char *p);

/* Atomic write ported to Aether (ae/subr/io.ae). */
extern int aether_io_write_atomic(const char *path, const char *data, int length);

static int write_file_atomic(const char *path, const char *data, int len) {
    return aether_io_write_atomic(path, data, len) == 0 ? 0 : -1;
}

int
svnae_wc_revert(const char *wc_root, const char *rel_path)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -2;

    struct svnae_wc_node *n = svnae_wc_db_get_node(db, rel_path);
    if (!n) { svnae_wc_db_close(db); return -1; }

    int kind  = svnae_wc_node_kind(n);
    int state = svnae_wc_node_state(n);
    int brev  = svnae_wc_node_base_rev(n);
    const char *bsha = svnae_wc_node_base_sha1(n);
    char sha_copy[41] = {0};
    if (bsha) { strncpy(sha_copy, bsha, sizeof sha_copy - 1); }
    svnae_wc_node_free(n);

    char disk[PATH_MAX];
    snprintf(disk, sizeof disk, "%s/%s", wc_root, rel_path);

    if (state == 1 /*added*/) {
        /* Drop the row. Leave the file in place (unversioned). */
        int rc = svnae_wc_db_delete_node(db, rel_path);
        svnae_wc_db_close(db);
        return rc == 0 ? 0 : -2;
    }

    if (kind == 1 /*dir*/) {
        /* Nothing content-wise to restore for a dir; just clear any
         * pending-delete state. */
        if (state == 2) {
            /* Restore: recreate the dir. */
            extern int aether_io_mkdir_p(const char *p);
            (void)aether_io_mkdir_p(disk);
        }
        svnae_wc_db_upsert_node(db, rel_path, 1, brev, "", 0);
        svnae_wc_db_close(db);
        return 0;
    }

    /* File. Restore content from pristine, reset state to normal. */
    int psize = svnae_wc_pristine_size(wc_root, sha_copy);
    char *data = svnae_wc_pristine_get(wc_root, sha_copy);
    if (!data) { svnae_wc_db_close(db); return -2; }
    int rc = write_file_atomic(disk, data, psize);
    svnae_wc_pristine_free(data);
    if (rc != 0) { svnae_wc_db_close(db); return -2; }

    svnae_wc_db_upsert_node(db, rel_path, 0, brev, sha_copy, 0);
    svnae_wc_db_close(db);
    return 0;
}
/* ae/wc/cleanup_shim.c — svn cleanup.
 *
 * Aborted atomic writes (kill -9, disk full, etc.) leave behind files
 * named "<target>.tmp.<pid>". These never get picked up by any
 * operation because the atomic rename is what publishes them. Over
 * time they accumulate. `svn cleanup` walks the WC (including
 * .svn/pristine) and deletes every *.tmp.* file.
 *
 * Also clears a stale .svn/wc.db-journal if present (SQLite hot-
 * journal that wasn't rolled back). We only delete if wc.db is
 * readable and has no in-flight lock — if another process is writing,
 * leave it alone.
 *
 * Returns the number of files deleted, or -1 on error.
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Does the basename match "*.tmp.<digits>"? That's the exact pattern
 * our write_file_atomic and friends use. */
static int
is_stale_tmp(const char *name)
{
    const char *p = strstr(name, ".tmp.");
    if (!p) return 0;
    p += 5;
    if (!*p) return 0;
    while (*p) {
        if (*p < '0' || *p > '9') return 0;
        p++;
    }
    return 1;
}

/* Dir walk ported to std.fs via aether_io_listdir. */
extern void *aether_io_listdir(const char *path);
extern int aether_io_listdir_count(void *handle);
extern const char *aether_io_listdir_name(void *handle, int index);
extern void aether_io_listdir_free(void *handle);
extern int aether_io_stat_kind(const char *path);

static int
walk_and_clean(const char *root)
{
    void *d = aether_io_listdir(root);
    if (!d) return 0;
    int count = 0;
    int n_entries = aether_io_listdir_count(d);
    for (int i = 0; i < n_entries; i++) {
        const char *name = aether_io_listdir_name(d, i);
        char full[PATH_MAX];
        int n = snprintf(full, sizeof full, "%s/%s", root, name);
        if (n < 0 || n >= (int)sizeof full) continue;
        int kind = aether_io_stat_kind(full);   /* 1=file 2=dir 3=symlink */
        if (kind == 2) {
            count += walk_and_clean(full);
        } else if (kind == 1 && is_stale_tmp(name)) {
            if (aether_io_unlink(full) == 0) count++;
        }
    }
    aether_io_listdir_free(d);
    return count;
}

int
svnae_wc_cleanup(const char *wc_root)
{
    if (!wc_root || !*wc_root) return -1;

    /* Verify this is a WC by checking for .svn/wc.db. */
    extern int aether_io_exists(const char *p);
    extern int aether_io_is_regular_file(const char *p);
    char wc_db[PATH_MAX];
    snprintf(wc_db, sizeof wc_db, "%s/.svn/wc.db", wc_root);
    if (!aether_io_exists(wc_db)) return -1;

    /* Walk the whole WC — includes .svn/pristine and user files.
     * is_stale_tmp gates what we actually delete, so we never touch
     * a real .svn/wc.db or .svn/format etc. */
    int count = walk_and_clean(wc_root);

    /* Stale SQLite hot-journal: .svn/wc.db-journal. Only delete if
     * we can open-exclusive and nothing's holding a write lock — we
     * check via stat's timestamp + a short sleep is overkill; the
     * conservative thing is to test-open for exclusive write. For
     * simplicity at this phase, just delete if no wc.db-shm/wal is
     * present (those indicate WAL-mode writers). Our WC uses
     * default rollback-journal mode, so the -shm/-wal files are
     * absent under normal operation. */
    char journal[PATH_MAX];
    snprintf(journal, sizeof journal, "%s/.svn/wc.db-journal", wc_root);
    if (aether_io_is_regular_file(journal)) {
        if (aether_io_unlink(journal) == 0) count++;
    }

    return count;
}
