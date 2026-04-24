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

/* ae/wc/merge3_shim.c — three-way text merge via diff3(1).
 *
 * The three inputs are buffers in memory (mine / base / theirs). We write
 * them to temp files, shell out to `diff3 -m` with appropriate --label
 * args, capture stdout, and return:
 *
 *   0  = clean merge (output has no conflict markers)
 *   1  = conflicts in output (caller writes the conflicted file,
 *        preserves the three inputs as sidecars, marks the node)
 *   -1 = error running diff3
 *
 * The merged bytes are returned via an out-param allocated with malloc;
 * the caller frees with svnae_merge3_free. Length via out_len.
 *
 * Labels used in markers so users see who's who:
 *   MINE    = the working-copy version (left side)
 *   BASE    = the common ancestor (middle)
 *   THEIRS  = the incoming version (right)
 *
 * For update:  mine=disk, base=pristine, theirs=remote-at-target.
 * For merge:   mine=disk-in-target, base=remote-source-at-A,
 *              theirs=remote-source-at-B.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Atomic write + slurp ported to Aether (ae/subr/io.ae). */
extern int aether_io_write_atomic(const char *path, const char *data, int length);
extern const char *aether_io_read_file(const char *path);
extern int aether_io_file_size(const char *path);

/* AetherString layout — the std.fs.read_binary path returns a
 * magic-tagged wrapper (aether ce5ef25 "header-leak fix"); treating
 * the return as a plain char* reads into the header. Unwrap here
 * the same way rep_store_shim.c does. */
#define AETHER_STRING_MAGIC 0xAE57C0DE
struct AetherString_local {
    unsigned int magic;
    int          ref_count;
    size_t       length;
    size_t       capacity;
    char        *data;
};

static int
write_tmp(const char *data, int len, char *out_path, size_t out_sz)
{
    static int seq = 0;
    snprintf(out_path, out_sz, "/tmp/svnae_m3_%d_%d.tmp", (int)getpid(), seq++);
    return aether_io_write_atomic(out_path, data, len) == 0 ? 0 : -1;
}

/* Slurp a file into malloc'd memory. Returns buf, sets *out_len. */
static char *
slurp(const char *path, int *out_len)
{
    int sz = aether_io_file_size(path);
    if (sz < 0) return NULL;
    const char *src = aether_io_read_file(path);
    if (!src) return NULL;

    const struct AetherString_local *as =
        (const struct AetherString_local *)src;
    const char *data = src;
    int len = sz;
    if (as->magic == AETHER_STRING_MAGIC) {
        data = as->data;
        len  = (int)as->length;
    }

    char *buf = malloc((size_t)len + 1);
    if (!buf) return NULL;
    if (len > 0) memcpy(buf, data, (size_t)len);
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

/* Run `diff3 -m --label MINE --label BASE --label THEIRS mine base theirs`
 * and capture stdout into a malloc'd buffer. Returns diff3's exit status
 * (0 clean, 1 conflicts, >1 error), or -1 on fork/wait failure. */
static int
run_diff3(const char *mine_path, const char *base_path, const char *theirs_path,
          char **out_merged, int *out_len)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        /* Child: redirect stdout to the write end; exec diff3. */
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        close(pipefd[1]);
        /* We don't need stderr unless diff3 explodes; let it through. */
        execlp("diff3", "diff3", "-m",
               "--label", "MINE",
               "--label", "BASE",
               "--label", "THEIRS",
               mine_path, base_path, theirs_path, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);

    /* Parent: drain pipe into a growable buf. */
    char *buf = NULL;
    int   cap = 0, len = 0;
    for (;;) {
        if (len + 4096 > cap) {
            int nc = cap ? cap * 2 : 8192;
            while (len + 4096 > nc) nc *= 2;
            buf = realloc(buf, (size_t)nc);
            cap = nc;
        }
        ssize_t n = read(pipefd[0], buf + len, 4096);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        len += (int)n;
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    int ex = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    if (ex < 0 || ex > 2) {
        free(buf);
        return -1;
    }
    if (!buf) buf = malloc(1);
    buf[len] = '\0';
    *out_merged = buf;
    *out_len = len;
    return ex;
}

/* Public entry point.
 *   mine/base/theirs — three buffers
 *   out_merged       — receives malloc'd merge result
 *   out_len          — its length
 * Returns 0 clean, 1 conflicts, -1 error. */
int
svnae_merge3(const char *mine,   int mine_len,
             const char *base,   int base_len,
             const char *theirs, int theirs_len,
             char **out_merged,  int *out_len)
{
    char mp[PATH_MAX], bp[PATH_MAX], tp[PATH_MAX];
    if (write_tmp(mine,   mine_len,   mp, sizeof mp)   != 0) return -1;
    if (write_tmp(base,   base_len,   bp, sizeof bp)   != 0) { aether_io_unlink(mp); return -1; }
    if (write_tmp(theirs, theirs_len, tp, sizeof tp)   != 0) { aether_io_unlink(mp); aether_io_unlink(bp); return -1; }

    int rc = run_diff3(mp, bp, tp, out_merged, out_len);

    aether_io_unlink(mp); aether_io_unlink(bp); aether_io_unlink(tp);
    return rc;
}

void svnae_merge3_free(char *p) { free(p); }

/* Convenience: apply 3-way merge to an on-disk WC file.
 *
 * Called when an update or merge operation wants to apply an incoming
 * `theirs` buffer against the WC file at `wc_path`, given a known base
 * content `base`. On a clean merge the file is overwritten with the
 * merged result. On conflicts the file is still overwritten (with the
 * marker-annotated text) and the three inputs are kept as sidecars:
 *
 *   <wc_path>.mine         — user's local version before the merge
 *   <wc_path>.r<base_rev>  — common-ancestor
 *   <wc_path>.r<theirs_rev>— incoming
 *
 * Caller is expected to flip wc.db's `conflicted` column on the node
 * when we return 1 so status/commit behave correctly.
 *
 * Returns 0 clean, 1 conflicts, -1 error. */
int
svnae_merge3_apply(const char *wc_path,
                   const char *base, int base_len, int base_rev,
                   const char *theirs, int theirs_len, int theirs_rev)
{
    int mine_len = 0;
    char *mine = slurp(wc_path, &mine_len);
    if (!mine) return -1;

    char *merged = NULL;
    int   merged_len = 0;
    int rc = svnae_merge3(mine, mine_len, base, base_len,
                          theirs, theirs_len, &merged, &merged_len);
    if (rc < 0) { free(mine); return -1; }

    /* Write merged back to wc_path, atomically. */
    if (aether_io_write_atomic(wc_path, merged, merged_len) != 0) {
        free(mine); free(merged); return -1;
    }

    if (rc == 1) {
        /* Conflicts — drop sidecars for svn resolve to use. The
         * sidecars don't need the atomic-rename dance, but going
         * through aether_io_write_atomic costs only an extra rename
         * and keeps one primitive in use. */
        char side[PATH_MAX];
        snprintf(side, sizeof side, "%s.mine", wc_path);
        (void)aether_io_write_atomic(side, mine, mine_len);
        snprintf(side, sizeof side, "%s.r%d", wc_path, base_rev);
        (void)aether_io_write_atomic(side, base, base_len);
        snprintf(side, sizeof side, "%s.r%d", wc_path, theirs_rev);
        (void)aether_io_write_atomic(side, theirs, theirs_len);
    }

    free(mine); free(merged);
    return rc;
}
