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

/* ae/wc/resolve_shim.c — svn resolve.
 *
 * Clears the `conflicted` flag on a WC node. Optionally chooses which
 * version becomes the working-copy content:
 *
 *   svnae_wc_resolve(wc_root, path, accept)
 *     accept 0 = "working"   — keep WC file as-is (user hand-resolved it)
 *     accept 1 = "mine-full" — replace WC file with the `.mine` sidecar
 *     accept 2 = "theirs-full" — replace WC file with `.r<theirs_rev>` sidecar
 *
 *   In all cases: sidecar files (`.mine`, `.r<N>`) are deleted and
 *   the node's conflicted=0 is persisted.
 *
 * Returns 0 on success, -1 if the path isn't conflicted or on error.
 */

#include <dirent.h>
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
int      svnae_wc_db_set_conflicted(sqlite3 *db, const char *path, int conflicted);

struct svnae_wc_node;
struct svnae_wc_node *svnae_wc_db_get_node(sqlite3 *db, const char *path);
int         svnae_wc_node_conflicted(const struct svnae_wc_node *n);
void        svnae_wc_node_free(struct svnae_wc_node *n);

/* Copy file src -> dst, overwriting if exists. */
static int
copy_overwrite(const char *src, const char *dst)
{
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dfd < 0) { close(sfd); return -1; }
    char buf[8192];
    for (;;) {
        ssize_t n = read(sfd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; close(sfd); close(dfd); return -1; }
        if (n == 0) break;
        const char *p = buf; ssize_t rem = n;
        while (rem > 0) {
            ssize_t w = write(dfd, p, (size_t)rem);
            if (w < 0) { if (errno == EINTR) continue; close(sfd); close(dfd); return -1; }
            p += w; rem -= w;
        }
    }
    close(sfd); close(dfd);
    return 0;
}

/* Delete any `basename.mine` and `basename.r<digits>` sidecars in the
 * containing directory. */
static void
remove_sidecars(const char *wc_root, const char *rel_path)
{
    char dirpath[PATH_MAX], base[256];
    /* Split rel_path at last '/'. */
    const char *slash = strrchr(rel_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - rel_path);
        if (dlen >= sizeof dirpath - 1) return;
        snprintf(dirpath, sizeof dirpath, "%s/%.*s", wc_root, (int)dlen, rel_path);
        snprintf(base, sizeof base, "%s", slash + 1);
    } else {
        snprintf(dirpath, sizeof dirpath, "%s", wc_root);
        snprintf(base, sizeof base, "%s", rel_path);
    }
    size_t blen = strlen(base);

    extern int aether_is_sidecar_suffix(const char *s);

    DIR *d = opendir(dirpath);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, base, blen) != 0) continue;
        if (e->d_name[blen] != '.') continue;
        if (!aether_is_sidecar_suffix(e->d_name + blen + 1)) continue;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s/%s", dirpath, e->d_name);
        unlink(full);
    }
    closedir(d);
}

/* Find the `<base>.r<N>` sidecar that isn't the base rev. There are
 * typically two: one for the common ancestor and one for theirs. We
 * take the larger N (which by construction is theirs). */
static int
find_theirs_sidecar(const char *wc_root, const char *rel_path, char *out, size_t out_sz)
{
    char dirpath[PATH_MAX], base[256];
    const char *slash = strrchr(rel_path, '/');
    if (slash) {
        size_t dlen = (size_t)(slash - rel_path);
        snprintf(dirpath, sizeof dirpath, "%s/%.*s", wc_root, (int)dlen, rel_path);
        snprintf(base, sizeof base, "%s", slash + 1);
    } else {
        snprintf(dirpath, sizeof dirpath, "%s", wc_root);
        snprintf(base, sizeof base, "%s", rel_path);
    }
    size_t blen = strlen(base);

    int best_rev = -1;
    DIR *d = opendir(dirpath);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, base, blen) != 0) continue;
        if (e->d_name[blen] != '.') continue;
        const char *suf = e->d_name + blen + 1;
        if (suf[0] != 'r' || !suf[1]) continue;
        int n = 0; int ok = 1;
        for (const char *q = suf + 1; *q; q++) {
            if (*q < '0' || *q > '9') { ok = 0; break; }
            n = n * 10 + (*q - '0');
        }
        if (ok && n > best_rev) best_rev = n;
    }
    closedir(d);
    if (best_rev < 0) return -1;
    snprintf(out, out_sz, "%s/%s.r%d", dirpath, base, best_rev);
    return 0;
}

int
svnae_wc_resolve(const char *wc_root, const char *rel_path, int accept)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;

    struct svnae_wc_node *n = svnae_wc_db_get_node(db, rel_path);
    if (!n) { svnae_wc_db_close(db); return -1; }
    int is_conflicted = svnae_wc_node_conflicted(n);
    svnae_wc_node_free(n);
    if (!is_conflicted) {
        svnae_wc_db_close(db);
        return -1;
    }

    char disk[PATH_MAX];
    snprintf(disk, sizeof disk, "%s/%s", wc_root, rel_path);

    if (accept == 1) {
        /* mine-full: copy .mine over the file. */
        char mine[PATH_MAX];
        snprintf(mine, sizeof mine, "%s.mine", disk);
        if (copy_overwrite(mine, disk) != 0) {
            svnae_wc_db_close(db); return -1;
        }
    } else if (accept == 2) {
        /* theirs-full: copy the highest-rev sidecar over the file. */
        char theirs[PATH_MAX];
        if (find_theirs_sidecar(wc_root, rel_path, theirs, sizeof theirs) != 0) {
            svnae_wc_db_close(db); return -1;
        }
        if (copy_overwrite(theirs, disk) != 0) {
            svnae_wc_db_close(db); return -1;
        }
    }
    /* accept == 0 (working): leave the file alone. */

    remove_sidecars(wc_root, rel_path);
    int rc = svnae_wc_db_set_conflicted(db, rel_path, 0);
    svnae_wc_db_close(db);
    return rc == 0 ? 0 : -1;
}
