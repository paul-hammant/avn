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

/* ae/svnadmin/shim.c — fd I/O primitives for svnadmin dump/load.
 *
 * The dump/load orchestration lives in ae/svnadmin/dump_load.ae;
 * what remains here is the read(2)/write(2) plumbing that std.fs
 * doesn't expose:
 *
 *   svnae_fd_write_str(fd, str) — write the full string, retry EINTR
 *   svnae_fd_write_rep_body(fd, repo, sha, size) — slurp + write a
 *     binary rep blob, then a trailing newline
 *   svnae_fd_read_line(fd) — read one '\n'-delimited line into a TLS
 *     buffer, return as Aether string ("" on EOF)
 *   svnae_fd_load_rep_block(fd, repo, expected_sha, size) — read
 *     `size` bytes + the trailing '\n', call svnae_rep_write_blob,
 *     verify the returned hash matches `expected_sha`. Returns 0
 *     ok / -1 on any error or hash mismatch.
 *
 * Plus svnae_fsfs_now_iso8601 (TLS-cached UTC timestamp wrapping
 * std.os's raw extern). */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern const char *svnae_rep_write_blob(const char *repo, const char *data, int len);

/* Write the full string to fd, retrying on EINTR. Returns 0 ok, -1 fail. */
static int
write_all(int fd, const char *data, int len)
{
    const char *p = data; int rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; rem -= (int)w;
    }
    return 0;
}

/* Read exactly `len` bytes from fd. Returns 0 on success, -1 on
 * short read or error. */
static int
read_n(int fd, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, (size_t)(len - got));
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        got += (int)n;
    }
    return 0;
}

int
svnae_fd_write_str(int fd, const char *s)
{
    if (!s) return 0;
    return write_all(fd, s, (int)strlen(s));
}

extern char *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
extern void  svnae_rep_free(char *p);

int
svnae_fd_write_rep_body(int fd, const char *repo, const char *sha, int size)
{
    char *bytes = svnae_rep_read_blob(repo, sha);
    if (!bytes) return -1;
    int rc = write_all(fd, bytes, size);
    svnae_rep_free(bytes);
    return rc;
}

/* Read one '\n'-delimited line from fd into a TLS buffer. Returns
 * the buffer (NUL-terminated, no trailing '\n') as a const char *
 * Aether will treat as a string. Returns "" on EOF or read error. */
const char *
svnae_fd_read_line(int fd)
{
    static __thread char buf[1024];
    int i = 0;
    while (i < (int)sizeof buf - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) { if (errno == EINTR) continue; buf[0] = '\0'; return buf; }
        if (n == 0) { if (i == 0) { buf[0] = '\0'; return buf; } break; }
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return buf;
}

/* Read `size` rep-block bytes + trailing newline from fd, write the
 * blob via svnae_rep_write_blob, verify the returned hex matches
 * `expected_sha`. Returns 0 ok, -1 on any error or hash mismatch. */
int
svnae_fd_load_rep_block(int fd, const char *repo,
                        const char *expected_sha, int size)
{
    char *data = malloc((size_t)size + 1);
    if (!data) return -1;
    if (read_n(fd, data, size) != 0) { free(data); return -1; }
    data[size] = '\0';

    char nl;
    if (read_n(fd, &nl, 1) != 0 || nl != '\n') { free(data); return -1; }

    const char *written = svnae_rep_write_blob(repo, data, size);
    free(data);
    if (!written || strcmp(written, expected_sha) != 0) return -1;
    return 0;
}
