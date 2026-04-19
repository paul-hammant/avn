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

static int
write_file_atomic(const char *path, const char *data, int len)
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
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -errno; }
    return 0;
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
            mkdir(disk, 0755);
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
