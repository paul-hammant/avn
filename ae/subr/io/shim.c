/* io/shim.c — file operations that std.fs doesn't cover.
 *
 * svn writes a lot of data to disk safely: revisions, working-copy pristines,
 * rep-cache blobs. The pattern is always the same — write to a temp path in
 * the same directory, fsync, rename over the target. std.fs has open/read/
 * write but no rename, no fsync, and no way to ask for the process PID (we
 * use that to build unique tmp names). This shim fills those gaps.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* rename(2): returns 0 on success, -errno on failure. */
int
svnae_rename(const char *from, const char *to)
{
    if (rename(from, to) == 0) return 0;
    return -errno;
}

/* Open `path` with O_WRONLY|O_CREAT|O_TRUNC, write all bytes, fsync, close.
 * Returns 0 on success, -errno on failure. The caller is expected to have
 * constructed a safe (in-directory) path. */
int
svnae_write_and_fsync(const char *path, const char *data, int len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;

    const char *p = data;
    int remaining = len;
    while (remaining > 0) {
        ssize_t w = write(fd, p, (size_t)remaining);
        if (w < 0) {
            if (errno == EINTR) continue;
            int rc = -errno;
            close(fd);
            return rc;
        }
        p += w;
        remaining -= (int)w;
    }

    if (fsync(fd) != 0) {
        int rc = -errno;
        close(fd);
        return rc;
    }
    if (close(fd) != 0) return -errno;
    return 0;
}

/* Return the current PID (for building unique tmp filenames). */
int svnae_getpid(void) { return (int)getpid(); }

/* Does the file exist as a regular file? */
int
svnae_is_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/* Remove a file (not a directory). Returns 0 on success, -errno on failure.
 * Returns 0 if the file did not exist (ENOENT) — idempotent, like svn's
 * "remove if present" semantics for tmp cleanup paths. */
int
svnae_remove(const char *path)
{
    if (unlink(path) == 0) return 0;
    if (errno == ENOENT) return 0;
    return -errno;
}
