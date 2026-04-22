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

/* ae/wc/checkout_shim.c — svn checkout.
 *
 * Checks out a tree from a remote URL into a local working copy directory.
 *   svnae_wc_checkout(base_url, repo_name, dest, rev) -> 0 on success
 *
 * Algorithm:
 *   1. mkdir -p dest; init wc.db; record url + repo_name + base_rev in info.
 *   2. Walk the tree recursively via RA list(rev, path):
 *        - For each entry at dir_path:
 *            - if kind=="file": RA cat it, write to $dest/<dir>/<name>,
 *              pristine_put the bytes, upsert nodes(path, file, rev, sha1, 0).
 *            - if kind=="dir": mkdir $dest/<dir>/<name>, upsert dir row,
 *              recurse into that path.
 *
 * Reads use the Phase 7 RA (cross-HTTP); writes go to local pristine/db.
 * For binary file contents cat returns a malloc'd NUL-terminated buffer —
 * we use strlen to determine the length. When binary blobs flow end-to-end
 * (deferred item in the port), swap this for a length-aware ra_cat.
 */

#include <errno.h>
#include <fcntl.h>
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

/* --- imports from the other shims ------------------------------------- */

struct svnae_ra_list;

struct svnae_ra_list *svnae_ra_list(const char *base_url, const char *repo_name, int rev, const char *path);
int         svnae_ra_list_count(const struct svnae_ra_list *L);
const char *svnae_ra_list_name(const struct svnae_ra_list *L, int i);
const char *svnae_ra_list_kind(const struct svnae_ra_list *L, int i);
void        svnae_ra_list_free(struct svnae_ra_list *L);

char *svnae_ra_cat(const char *base_url, const char *repo_name, int rev, const char *path);
void  svnae_ra_free(char *p);

sqlite3     *svnae_wc_db_open(const char *wc_root);
void         svnae_wc_db_close(sqlite3 *db);
int          svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int          svnae_wc_db_set_info(sqlite3 *db, const char *key, const char *value);

char *svnae_ra_hash_algo(const char *base_url, const char *repo_name);

const char *svnae_wc_pristine_put(const char *wc_root, const char *data, int len);

struct svnae_ra_props;
struct svnae_ra_props *svnae_ra_get_props(const char *base_url, const char *repo_name,
                                          int rev, const char *path);
int         svnae_ra_props_count(const struct svnae_ra_props *P);
const char *svnae_ra_props_name (const struct svnae_ra_props *P, int i);
const char *svnae_ra_props_value(const struct svnae_ra_props *P, int i);
void        svnae_ra_props_free (struct svnae_ra_props *P);

int svnae_wc_propset(const char *wc_root, const char *path,
                     const char *name, const char *value);

/* --- helpers ---------------------------------------------------------- */

static int
mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

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

/* Prefix/name join ported to Aether (ae/fs_fs/pathutil.ae). */
extern const char *aether_path_join_rel(const char *prefix, const char *name);

/* --- recursive walk --------------------------------------------------- */

/* Download everything at `prefix` (relative to repo root) into
 * `$dest/<prefix>`. Populates the db with rows as we go. */
static int
walk(const char *base_url, const char *repo, int rev,
     const char *dest, const char *prefix,
     sqlite3 *db)
{
    struct svnae_ra_list *L = svnae_ra_list(base_url, repo, rev, prefix);
    if (!L) return -1;
    int n = svnae_ra_list_count(L);

    for (int i = 0; i < n; i++) {
        const char *name = svnae_ra_list_name(L, i);
        const char *kind = svnae_ra_list_kind(L, i);

        /* repo-relative path (stored in wc.db). */
        const char *rel = aether_path_join_rel(prefix, name);
        /* local filesystem path under $dest. */
        const char *disk = aether_path_join_rel(dest, rel);

        if (strcmp(kind, "dir") == 0) {
            if (mkdir_p(disk) != 0) { svnae_ra_list_free(L); return -1; }
            svnae_wc_db_upsert_node(db, rel, 1 /*dir*/, rev, "", 0);
            /* Ingest props for this dir. */
            struct svnae_ra_props *P = svnae_ra_get_props(base_url, repo, rev, rel);
            if (P) {
                int pn = svnae_ra_props_count(P);
                for (int j = 0; j < pn; j++) {
                    svnae_wc_propset(dest, rel, svnae_ra_props_name(P, j),
                                                svnae_ra_props_value(P, j));
                }
                svnae_ra_props_free(P);
            }
            if (walk(base_url, repo, rev, dest, rel, db) != 0) {
                svnae_ra_list_free(L); return -1;
            }
        } else {
            char *data = svnae_ra_cat(base_url, repo, rev, rel);
            if (!data) { svnae_ra_list_free(L); return -1; }
            int len = (int)strlen(data);

            if (write_file_atomic(disk, data, len) != 0) {
                svnae_ra_free(data); svnae_ra_list_free(L); return -1;
            }
            const char *sha = svnae_wc_pristine_put(dest, data, len);
            if (!sha) { svnae_ra_free(data); svnae_ra_list_free(L); return -1; }
            svnae_wc_db_upsert_node(db, rel, 0 /*file*/, rev, sha, 0);
            svnae_ra_free(data);
            /* Ingest props for this file. */
            struct svnae_ra_props *P = svnae_ra_get_props(base_url, repo, rev, rel);
            if (P) {
                int pn = svnae_ra_props_count(P);
                for (int j = 0; j < pn; j++) {
                    svnae_wc_propset(dest, rel, svnae_ra_props_name(P, j),
                                                svnae_ra_props_value(P, j));
                }
                svnae_ra_props_free(P);
            }
        }
    }
    svnae_ra_list_free(L);
    return 0;
}

/* Main entry point. Creates $dest if needed; initialises wc.db; walks
 * the server tree at `rev`, materialising every file and directory.
 * Returns 0 on success. */
int
svnae_wc_checkout(const char *base_url, const char *repo_name,
                  const char *dest, int rev)
{
    if (mkdir_p(dest) != 0) return -1;

    sqlite3 *db = svnae_wc_db_open(dest);
    if (!db) return -1;

    /* Record the connection info so later commands don't need it.
     * `url` is the full URL the user typed (base + "/" + repo). `base_url`
     * and `repo` are also kept separately so other WC commands don't have
     * to re-split. */
    char full_url[1024];
    snprintf(full_url, sizeof full_url, "%s/%s", base_url, repo_name);
    svnae_wc_db_set_info(db, "url",      full_url);
    svnae_wc_db_set_info(db, "base_url", base_url);
    svnae_wc_db_set_info(db, "repo",     repo_name);
    char rev_str[16]; snprintf(rev_str, sizeof rev_str, "%d", rev);
    svnae_wc_db_set_info(db, "base_rev", rev_str);

    /* Record the server's content-address algo so this WC's local
     * pristine store and modification-detection logic match. Defaults
     * to sha1 if the server doesn't advertise (pre-Phase-6.1 server). */
    char *algo = svnae_ra_hash_algo(base_url, repo_name);
    svnae_wc_db_set_info(db, "hash_algo", algo ? algo : "sha1");
    free(algo);

    int rc = walk(base_url, repo_name, rev, dest, "", db);

    svnae_wc_db_close(db);
    return rc;
}
