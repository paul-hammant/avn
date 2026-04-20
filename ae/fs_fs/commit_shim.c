/* fs_fs/commit_shim.c — finalise a txn into a revision.
 *
 * The recursive tree-rebuild lives in txn_shim.c (svnae_txn_rebuild_root).
 * This file wraps that with the last steps: build the revision blob,
 * write revs/NNNNNN pointer, bump $repo/head. A single entry point
 * callable both from Aether and from the HTTP server handler.
 *
 * Returns the new revision number on success, -1 on failure.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

struct svnae_txn;

/* Forward-declared from other shims we depend on. */
int   svnae_txn_base_rev(const struct svnae_txn *t);
char *svnae_txn_rebuild_root(const char *repo, const char *base_root_sha1,
                             const struct svnae_txn *t);
const char *svnae_rep_write_blob(const char *repo, const char *data, int len);
char       *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
void        svnae_rep_free(char *p);
const char *svnae_fsfs_now_iso8601(void);
int         svnae_repos_head_rev(const char *repo);

/* Atomic write + fsync — re-used here so we don't introduce a dep edge
 * from this shim back to the one that owns the util. */
static int
write_atomic(const char *path, const char *data, int len)
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
    if (close(fd) != 0) return -errno;
    if (rename(tmp, path) != 0) { unlink(tmp); return -errno; }
    return 0;
}

/* Read the root-dir sha1 for a given revision. Returns malloc'd string
 * or NULL. */
static char *
load_rev_root_sha1(const char *repo, int rev)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/revs/%06d", repo, rev);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[128];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return NULL; }
    fclose(f);
    /* trim trailing whitespace */
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = '\0';

    char *rev_body = svnae_rep_read_blob(repo, buf);
    if (!rev_body) return NULL;
    /* Parse "root: <sha1>" line. */
    const char *key = "root: ";
    char *p = strstr(rev_body, key);
    if (!p) { svnae_rep_free(rev_body); return NULL; }
    p += strlen(key);
    const char *eol = strchr(p, '\n');
    size_t len = eol ? (size_t)(eol - p) : strlen(p);
    char *out = malloc(len + 1);
    memcpy(out, p, len);
    out[len] = '\0';
    svnae_rep_free(rev_body);
    return out;
}

/* Finalise: take the txn's base rev, rebuild the tree, write a new
 * revision blob, bump head, return the new rev number. -1 on failure.
 *
 * `props_sha1` is the sha1 of a paths-props blob associated with this
 * commit — pass an empty string to inherit the previous rev's props
 * (rep-sharing works automatically), or a specific sha1 when commits
 * touch properties. This extension is additive: older clients/tests
 * that pass "" always get the previous rev's props-sha1 carried
 * forward. */

/* Read a "key: <value>\n" line from an existing revision blob (or
 * return NULL if not present). Caller frees. */
static char *
rev_blob_field(const char *repo, int rev, const char *key)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/revs/%06d", repo, rev);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[128];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return NULL; }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';

    char *body = svnae_rep_read_blob(repo, buf);
    if (!body) return NULL;
    size_t klen = strlen(key);
    char needle[64];
    snprintf(needle, sizeof needle, "%s: ", key);
    char *p = strstr(body, needle);
    char *out = NULL;
    if (p) {
        p += klen + 2;
        char *eol = strchr(p, '\n');
        size_t L = eol ? (size_t)(eol - p) : strlen(p);
        out = malloc(L + 1);
        memcpy(out, p, L);
        out[L] = '\0';
    }
    svnae_rep_free(body);
    return out;
}

int
svnae_commit_finalise(const char *repo, struct svnae_txn *txn,
                      const char *author, const char *logmsg)
{
    return svnae_commit_finalise_with_props(repo, txn, author, logmsg, "");
}

/* Build a per-path props blob ("key=value\n" lines), write it to the
 * rep store, return its sha1 (as static thread-local, copy before
 * reuse). NULL on failure. */
const char *
svnae_build_props_blob(const char *repo,
                      const char *const *keys,
                      const char *const *values,
                      int n_pairs)
{
    /* Sort (key, value) pairs by key so identical prop sets always
     * hash to the same sha1. Cheap: bubble sort on indices. */
    int *order = malloc(sizeof(int) * (size_t)(n_pairs > 0 ? n_pairs : 1));
    for (int i = 0; i < n_pairs; i++) order[i] = i;
    for (int i = 0; i < n_pairs; i++) {
        for (int j = i + 1; j < n_pairs; j++) {
            if (strcmp(keys[order[i]], keys[order[j]]) > 0) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    body[0] = '\0';
    for (int i = 0; i < n_pairs; i++) {
        int idx = order[i];
        size_t need = strlen(keys[idx]) + 1 + strlen(values[idx]) + 1 + 1;
        if (len + need >= cap) {
            cap = (len + need) * 2;
            body = realloc(body, cap);
        }
        len += (size_t)snprintf(body + len, cap - len, "%s=%s\n", keys[idx], values[idx]);
    }
    free(order);
    const char *sha = svnae_rep_write_blob(repo, body, (int)len);
    free(body);
    return sha;   /* static-thread-local in rep_store_shim */
}

/* Build a paths-props blob ("<props-sha1> <path>\n" per line, sorted
 * by path), write to rep store, return its sha1. */
const char *
svnae_build_paths_props_blob(const char *repo,
                            const char *const *paths,
                            const char *const *props_shas,
                            int n_paths)
{
    int *order = malloc(sizeof(int) * (size_t)(n_paths > 0 ? n_paths : 1));
    for (int i = 0; i < n_paths; i++) order[i] = i;
    for (int i = 0; i < n_paths; i++) {
        for (int j = i + 1; j < n_paths; j++) {
            if (strcmp(paths[order[i]], paths[order[j]]) > 0) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    body[0] = '\0';
    for (int i = 0; i < n_paths; i++) {
        int idx = order[i];
        size_t need = 40 + 1 + strlen(paths[idx]) + 1 + 1;
        if (len + need >= cap) {
            cap = (len + need) * 2;
            body = realloc(body, cap);
        }
        len += (size_t)snprintf(body + len, cap - len, "%s %s\n",
                                props_shas[idx], paths[idx]);
    }
    free(order);
    const char *sha = svnae_rep_write_blob(repo, body, (int)len);
    free(body);
    return sha;
}

int
svnae_commit_finalise_with_props(const char *repo, struct svnae_txn *txn,
                                 const char *author, const char *logmsg,
                                 const char *props_sha1)
{
    int base_rev = svnae_txn_base_rev(txn);
    if (base_rev < 0) return -1;
    char *base_root = load_rev_root_sha1(repo, base_rev);
    if (!base_root) return -1;

    char *new_root = svnae_txn_rebuild_root(repo, base_root, txn);
    free(base_root);
    if (!new_root) return -1;

    int prev = svnae_repos_head_rev(repo);
    if (prev < 0) { free(new_root); return -1; }
    int next = prev + 1;

    /* If caller didn't supply a props_sha1, inherit the previous rev's. */
    char *inherited_props = NULL;
    const char *ps = props_sha1 && *props_sha1 ? props_sha1 : NULL;
    if (!ps) {
        inherited_props = rev_blob_field(repo, prev, "props");
        ps = inherited_props ? inherited_props : "";
    }

    /* Build revision blob. */
    size_t est = strlen(author) + strlen(logmsg) + 128 + 40 + 40 + 32;
    char *rev_body = malloc(est);
    int n = snprintf(rev_body, est,
                     "root: %s\nprops: %s\nprev: %d\nauthor: %s\ndate: %s\nlog: %s\n",
                     new_root, ps, prev, author, svnae_fsfs_now_iso8601(), logmsg);
    if (n >= (int)est) {
        free(rev_body);
        rev_body = malloc((size_t)n + 1);
        snprintf(rev_body, (size_t)n + 1,
                 "root: %s\nprops: %s\nprev: %d\nauthor: %s\ndate: %s\nlog: %s\n",
                 new_root, ps, prev, author, svnae_fsfs_now_iso8601(), logmsg);
    }
    free(inherited_props);

    const char *rev_sha = svnae_rep_write_blob(repo, rev_body, (int)strlen(rev_body));
    free(rev_body);
    free(new_root);
    if (!rev_sha) return -1;

    /* revs/NNNNNN pointer file. */
    char ptr_path[PATH_MAX];
    snprintf(ptr_path, sizeof ptr_path, "%s/revs/%06d", repo, next);
    char ptr_body[128];   /* sha256 hex is 64; keep some slack */
    int plen = snprintf(ptr_body, sizeof ptr_body, "%s\n", rev_sha);
    if (write_atomic(ptr_path, ptr_body, plen) != 0) return -1;

    /* head. */
    char head_path[PATH_MAX];
    snprintf(head_path, sizeof head_path, "%s/head", repo);
    char head_body[32];
    int hlen = snprintf(head_body, sizeof head_body, "%d\n", next);
    if (write_atomic(head_path, head_body, hlen) != 0) return -1;

    return next;
}
