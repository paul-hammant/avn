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

/* Unbuffered writer to stderr (fd 2). Aether's println buffers, so when
 * debugging a crash we lose everything between the last flush and the
 * SIGSEGV — this gives test code an escape hatch. */
int svnae_stderr_write(const char *buf, int n) {
    ssize_t w = write(2, buf, (size_t)n);
    return (int)w;
}

/* Same for stdout, for the CLI's binary-safe cat output. */
int svnae_stdout_write(const char *buf, int n) {
    ssize_t w = write(1, buf, (size_t)n);
    return (int)w;
}

/* File-descriptor helpers for CLI tools that need to pass fd numbers to
 * shim functions (e.g. svnadmin dump --file OUT, load --file IN). */
int open_file_for_write(const char *path) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
}
int open_file_for_read(const char *path) {
    return open(path, O_RDONLY);
}
int close_fd(int fd) {
    return close(fd);
}

int svnae_stderr_write_int(int v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", v);
    (void)write(2, buf, (size_t)n);
    return 0;
}

/* Debug: write strlen(p) and first 30 bytes (hex) of p to stderr. */
int svnae_debug_dump(const char *label, const void *p) {
    char buf[256];
    int n = snprintf(buf, sizeof buf, "DEBUG %s: ptr=%p strlen=%zu\n", label, p, strlen((const char*)p));
    (void)write(2, buf, (size_t)n);
    n = snprintf(buf, sizeof buf, "  last-30-bytes: ");
    (void)write(2, buf, (size_t)n);
    const char *cp = (const char*)p;
    size_t sl = strlen(cp);
    const char *start = (sl > 30) ? cp + sl - 30 : cp;
    for (const char *q = start; q < cp + sl; q++) {
        n = snprintf(buf, sizeof buf, "%02x ", (unsigned char)*q);
        (void)write(2, buf, (size_t)n);
    }
    (void)write(2, "\n", 1);
    return 0;
}

/* Write `header` (a NUL-terminated Aether string) immediately followed by
 * `body_len` bytes at `body` (which may contain embedded NULs).
 * header is treated as a single 1-byte prefix here — we only need the
 * first byte in practice, and taking strlen of a known "R"/"Z" is safe. */
int
svnae_write_header_and_body(const char *path, const char *header,
                            const char *body, int body_len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;

    int header_len = (int)strlen(header);
    const char *p = header;
    int rem = header_len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -errno; }
        p += w; rem -= (int)w;
    }

    p = body;
    rem = body_len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -errno; }
        p += w; rem -= (int)w;
    }

    if (fsync(fd) != 0) { int rc = -errno; close(fd); return rc; }
    if (close(fd) != 0) return -errno;
    return 0;
}

/* Does the file exist as a regular file? */
int
svnae_is_regular_file(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

/* Read the entire file into an svnae_buf. Binary-safe (embedded NULs OK).
 * Returns NULL on any failure. The returned buf is owned by the caller;
 * free with svnae_buf_free (from compress/shim — same struct layout).
 *
 * We don't depend on compress/shim.c being linked: define our own local
 * svnae_buf type here with identical layout. Aether sees it as `ptr`. */
struct svnae_buf_local {
    char *data;
    int   length;
};

/* out_len is optional — callers may pass NULL when they'll read the
 * length via svnae_read_file_length() off the returned handle instead. */
void *
svnae_read_file(const char *path, int *out_len)
{
    if (out_len) *out_len = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    if (!S_ISREG(st.st_mode)) { close(fd); return NULL; }

    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    if (!buf) { close(fd); return NULL; }

    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf); close(fd); return NULL;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);

    if (got != size) { free(buf); return NULL; }
    buf[size] = '\0';

    struct svnae_buf_local *b = malloc(sizeof *b);
    if (!b) { free(buf); return NULL; }
    b->data = buf;
    b->length = (int)size;
    if (out_len) *out_len = (int)size;
    return b;
}

int         svnae_read_file_length(const struct svnae_buf_local *b) { return b ? b->length : 0; }
const char *svnae_read_file_data  (const struct svnae_buf_local *b) { return b ? b->data : ""; }
void        svnae_read_file_free  (struct svnae_buf_local *b)
{
    if (!b) return;
    free(b->data);
    free(b);
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
