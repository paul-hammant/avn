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

/* io/shim.c — fd-level file operations that std.fs doesn't expose:
 * direct open() returning a fd (for svnadmin dump/load), unbuffered
 * writes to stdout/stderr (escape hatch for test traces around
 * std.println's buffering), and getpid (for tmp-filename
 * construction). Pure file I/O — atomic-write, mkdir-p, rename,
 * unlink, header+body atomic write — lives in subr/io.ae via
 * std.fs + std.bytes. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* svnae_getpid retired in Round 164 — std.os exposes os.getpid()
 * (asked for in std_remaining_gaps.md, shipped in Aether
 * [current]). Callers reach `os_getpid_raw` via inline extern. */

/* Unbuffered writer to stderr (fd 2). Aether's println buffers, so when
 * debugging a crash we lose everything between the last flush and the
 * SIGSEGV — this gives test code an escape hatch. */
int svnae_stderr_write(const char *buf, int n) {
    ssize_t w = write(2, buf, (size_t)n);
    return (int)w;
}

/* NUL-terminated stderr write. Wraps svnae_stderr_write for Aether
 * callers where passing a length is awkward (short trace strings). */
int svnae_stderr_puts(const char *s) {
    if (!s) return 0;
    return svnae_stderr_write(s, (int)strlen(s));
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

/* Read the entire file into an svnae_buf_local (binary-safe). Returns
 * NULL on any failure. The struct layout matches svnae_buf elsewhere
 * but is private here so this shim doesn't depend on compress/shim.c
 * being linked. Aether sees it as `ptr`. */
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
