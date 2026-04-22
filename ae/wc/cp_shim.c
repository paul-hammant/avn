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

/* ae/wc/cp_shim.c — svn cp / mv.
 *
 * cp(src, dst):
 *   - src must be tracked and on disk
 *   - dst must NOT exist
 *   - copy bytes src -> dst on disk
 *   - upsert nodes(dst) with kind=src.kind, base_rev=0, base_sha1="",
 *     state=added
 *   Pristine store already has the content (it's the same blob under
 *   the same sha1), so nothing else to write.
 *
 * mv(src, dst):
 *   - cp(src, dst), then rm(src). One atomic-ish sequence.
 *
 * For Phase 5.9 we only handle files. Tree copies (recursive) come
 * later; they're the same algorithm applied over a walk.
 *
 * API:
 *   svnae_wc_cp(wc_root, src, dst) -> 0 ok
 *                                      -1 src not tracked
 *                                      -2 dst already exists
 *                                      -3 src not on disk
 *                                      -4 other error
 *   svnae_wc_mv(wc_root, src, dst) -> same
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

sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int      svnae_wc_db_node_exists(sqlite3 *db, const char *path);

struct svnae_wc_node;
struct svnae_wc_node *svnae_wc_db_get_node(sqlite3 *db, const char *path);
int         svnae_wc_node_kind(const struct svnae_wc_node *n);
void        svnae_wc_node_free(struct svnae_wc_node *n);

int svnae_wc_rm(const char *wc_root, const char *rel_path);

/* Copy file src -> dst on disk. Returns 0 on success. */
static int
copy_file(const char *src, const char *dst)
{
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -errno;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (dfd < 0) { int rc = -errno; close(sfd); return rc; }
    char buf[8192];
    for (;;) {
        ssize_t n = read(sfd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; int rc = -errno; close(sfd); close(dfd); unlink(dst); return rc; }
        if (n == 0) break;
        const char *p = buf;
        ssize_t rem = n;
        while (rem > 0) {
            ssize_t w = write(dfd, p, (size_t)rem);
            if (w < 0) { if (errno == EINTR) continue; int rc = -errno; close(sfd); close(dfd); unlink(dst); return rc; }
            p += w; rem -= w;
        }
    }
    if (fsync(dfd) != 0) { int rc = -errno; close(sfd); close(dfd); unlink(dst); return rc; }
    close(sfd);
    close(dfd);
    return 0;
}

int
svnae_wc_cp(const char *wc_root, const char *src, const char *dst)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -4;

    /* src tracked? */
    struct svnae_wc_node *sn = svnae_wc_db_get_node(db, src);
    if (!sn) { svnae_wc_db_close(db); return -1; }
    int src_kind = svnae_wc_node_kind(sn);
    svnae_wc_node_free(sn);
    if (src_kind != 0) {
        /* directory copy not supported in Phase 5.9 */
        svnae_wc_db_close(db);
        return -4;
    }

    /* dst not already tracked AND not on disk. */
    if (svnae_wc_db_node_exists(db, dst)) { svnae_wc_db_close(db); return -2; }

    extern int aether_io_exists(const char *path);
    char src_disk[PATH_MAX]; snprintf(src_disk, sizeof src_disk, "%s/%s", wc_root, src);
    char dst_disk[PATH_MAX]; snprintf(dst_disk, sizeof dst_disk, "%s/%s", wc_root, dst);

    if (!aether_io_exists(src_disk)) { svnae_wc_db_close(db); return -3; }
    if (aether_io_exists(dst_disk))  { svnae_wc_db_close(db); return -2; }

    if (copy_file(src_disk, dst_disk) != 0) {
        svnae_wc_db_close(db);
        return -4;
    }

    /* Register dst as state=added. Content sha1 is unknown to the db at
     * this stage — we leave it empty; the next commit will compute it
     * from the on-disk file (same as any freshly-added path). */
    int rc = svnae_wc_db_upsert_node(db, dst, 0 /*file*/, 0, "", 1 /*added*/);
    svnae_wc_db_close(db);
    return rc == 0 ? 0 : -4;
}

int
svnae_wc_mv(const char *wc_root, const char *src, const char *dst)
{
    int rc = svnae_wc_cp(wc_root, src, dst);
    if (rc != 0) return rc;
    /* If the rm fails we've left a half-finished state (dst copy with
     * src still tracked). Phase 5.9 best-effort: try the rm; on
     * failure, log and return. */
    rc = svnae_wc_rm(wc_root, src);
    if (rc != 0) return -4;
    return 0;
}
