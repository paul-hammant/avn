/* ae/wc/merge_shim.c — svn merge (linear rev-range forward merge).
 *
 * Scope:
 *   svnae_wc_merge(wc_root, source_url_path, revA, revB)
 *     Apply the file-tree diff between revA and revB (of source_url_path
 *     on the WC's home server) into the WC. Writes bytes, queues add/rm
 *     in wc.db so the next commit pushes them. Records the merged range
 *     in the svn:mergeinfo property on the WC's root tracked node (or
 *     a sentinel we upsert if the root isn't tracked).
 *
 * Scope limits for Phase 5.12:
 *   - No conflict resolution. If a local file is already modified
 *     (disk sha != pristine sha) we abort with -2 BEFORE changing anything.
 *   - Merge source + target are the full WC (not a subpath). Reference
 *     svn lets you merge into any directory; we always merge into "."
 *     and require the user to cd to the right place.
 *   - mergeinfo ranges are tracked as a simple "<source-path>:revA-revB"
 *     newline-separated list. No subtraction of prior merges yet — we
 *     just append. A follow-up phase can do range arithmetic.
 *
 * The WC's existing base_url/repo is read from wc.db info. The
 * source_url_path is relative to the repo root — e.g. "trunk".
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
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

/* Externs we use. */
sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int      svnae_wc_db_delete_node(sqlite3 *db, const char *path);
int      svnae_wc_db_node_exists(sqlite3 *db, const char *path);

struct svnae_wc_node;
struct svnae_wc_node *svnae_wc_db_get_node(sqlite3 *db, const char *path);
int         svnae_wc_node_kind(const struct svnae_wc_node *n);
const char *svnae_wc_node_base_sha1(const struct svnae_wc_node *n);
int         svnae_wc_node_base_rev(const struct svnae_wc_node *n);
void        svnae_wc_node_free(struct svnae_wc_node *n);

char *svnae_wc_db_get_info(sqlite3 *db, const char *key);
void  svnae_wc_info_free(char *s);

const char *svnae_wc_pristine_put(const char *wc_root, const char *data, int len);

int   svnae_ra_head_rev(const char *base_url, const char *repo_name);
struct svnae_ra_list;
struct svnae_ra_list *svnae_ra_list(const char *base_url, const char *repo_name, int rev, const char *path);
int         svnae_ra_list_count(const struct svnae_ra_list *L);
const char *svnae_ra_list_name(const struct svnae_ra_list *L, int i);
const char *svnae_ra_list_kind(const struct svnae_ra_list *L, int i);
void        svnae_ra_list_free(struct svnae_ra_list *L);
char       *svnae_ra_cat(const char *base_url, const char *repo_name, int rev, const char *path);
void        svnae_ra_free(char *p);

int   svnae_wc_propset(const char *wc_root, const char *path, const char *name, const char *value);
char *svnae_wc_propget(const char *wc_root, const char *path, const char *name);
void  svnae_wc_props_free(char *s);

int   svnae_wc_db_set_conflicted(sqlite3 *db, const char *path, int conflicted);
int   svnae_merge3_apply(const char *wc_path,
                         const char *base, int base_len, int base_rev,
                         const char *theirs, int theirs_len, int theirs_rev);

/* --- tiny helpers ---------------------------------------------------- */

static int
sha1_of_file(const char *path, char out[41])
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    char buf[8192];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) { if (errno == EINTR) continue; EVP_MD_CTX_free(ctx); close(fd); return -1; }
        if (n == 0) break;
        EVP_DigestUpdate(ctx, buf, (size_t)n);
    }
    close(fd);
    unsigned char dig[EVP_MAX_MD_SIZE]; unsigned int dlen = 0;
    EVP_DigestFinal_ex(ctx, dig, &dlen);
    EVP_MD_CTX_free(ctx);
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 20; i++) {
        out[i*2]   = hex[dig[i] >> 4];
        out[i*2+1] = hex[dig[i] & 0xf];
    }
    out[40] = '\0';
    return 0;
}

static void
sha1_of_bytes(const char *data, int len, char out[41])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char dig[EVP_MAX_MD_SIZE]; unsigned int dlen = 0;
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, data, (size_t)len);
    EVP_DigestFinal_ex(ctx, dig, &dlen);
    EVP_MD_CTX_free(ctx);
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 20; i++) {
        out[i*2]   = hex[dig[i] >> 4];
        out[i*2+1] = hex[dig[i] & 0xf];
    }
    out[40] = '\0';
}

static int
mkdir_p(const char *path)
{
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", path);
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
    const char *p = data; int rem = len;
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

/* --- remote-tree snapshot (same as update_shim) ---------------------- */

struct rnode { char *path; int kind; char sha1[41]; char *data; int data_len; };
struct rtree { struct rnode *items; int n, cap; };

static void
rt_add(struct rtree *rt, const char *path, int kind, const char *data, int dlen)
{
    if (rt->n == rt->cap) {
        int nc = rt->cap ? rt->cap * 2 : 16;
        rt->items = realloc(rt->items, (size_t)nc * sizeof *rt->items);
        rt->cap = nc;
    }
    struct rnode *e = &rt->items[rt->n++];
    e->path = strdup(path);
    e->kind = kind;
    e->sha1[0] = '\0';
    if (kind == 0 && data) {
        e->data = malloc((size_t)dlen + 1);
        memcpy(e->data, data, (size_t)dlen);
        e->data[dlen] = '\0';
        e->data_len = dlen;
        sha1_of_bytes(data, dlen, e->sha1);
    } else {
        e->data = NULL;
        e->data_len = 0;
    }
}

static void
rt_free(struct rtree *rt)
{
    for (int i = 0; i < rt->n; i++) { free(rt->items[i].path); free(rt->items[i].data); }
    free(rt->items);
}

static const struct rnode *
rt_find(const struct rtree *rt, const char *path)
{
    for (int i = 0; i < rt->n; i++) if (strcmp(rt->items[i].path, path) == 0) return &rt->items[i];
    return NULL;
}

/* Walk a subtree under `source_path` at `rev` on the remote. The stored
 * paths are RELATIVE TO source_path — so a merge of source_path=trunk
 * yields entries like "main.c", "lib/util.c" which we can apply in the
 * current WC (which was checked out at repo root). */
static int
walk_remote(const char *base_url, const char *repo, int rev,
            const char *source_path, const char *sub_prefix,
            struct rtree *rt)
{
    char full[PATH_MAX];
    if (*sub_prefix) snprintf(full, sizeof full, "%s/%s", source_path, sub_prefix);
    else             snprintf(full, sizeof full, "%s",    source_path);

    struct svnae_ra_list *L = svnae_ra_list(base_url, repo, rev, full);
    if (!L) return -1;
    int n = svnae_ra_list_count(L);
    for (int i = 0; i < n; i++) {
        const char *name = svnae_ra_list_name(L, i);
        const char *kind = svnae_ra_list_kind(L, i);
        char rel[PATH_MAX];
        if (*sub_prefix) snprintf(rel, sizeof rel, "%s/%s", sub_prefix, name);
        else             snprintf(rel, sizeof rel, "%s",    name);

        if (strcmp(kind, "dir") == 0) {
            rt_add(rt, rel, 1, NULL, 0);
            if (walk_remote(base_url, repo, rev, source_path, rel, rt) != 0) {
                svnae_ra_list_free(L); return -1;
            }
        } else {
            char remote_full[PATH_MAX];
            if (*sub_prefix) snprintf(remote_full, sizeof remote_full, "%s/%s/%s", source_path, sub_prefix, name);
            else             snprintf(remote_full, sizeof remote_full, "%s/%s",    source_path, name);
            char *data = svnae_ra_cat(base_url, repo, rev, remote_full);
            if (!data) { svnae_ra_list_free(L); return -1; }
            rt_add(rt, rel, 0, data, (int)strlen(data));
            svnae_ra_free(data);
        }
    }
    svnae_ra_list_free(L);
    return 0;
}

/* --- mergeinfo plumbing ---------------------------------------------- */

/* Append "source:A-B\n" to the WC's svn:mergeinfo property on path ".". */
static int
mergeinfo_add_range(const char *wc_root, const char *source, int a, int b)
{
    /* We need a tracked path to hang props on. Use "" (the WC root) by
     * ensuring a dummy row exists if it doesn't already. */
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    if (!svnae_wc_db_node_exists(db, "")) {
        svnae_wc_db_upsert_node(db, "", 1 /*dir*/, 0, "", 0);
    }
    svnae_wc_db_close(db);

    char *existing = svnae_wc_propget(wc_root, "", "svn:mergeinfo");
    char line[256];
    snprintf(line, sizeof line, "%s:%d-%d", source, a + 1, b);
    char *new_val;
    if (existing && *existing) {
        size_t n = strlen(existing) + 1 + strlen(line) + 1;
        new_val = malloc(n);
        snprintf(new_val, n, "%s\n%s", existing, line);
    } else {
        new_val = strdup(line);
    }
    svnae_wc_props_free(existing);
    int rc = svnae_wc_propset(wc_root, "", "svn:mergeinfo", new_val);
    free(new_val);
    return rc;
}

/* --- main merge entry point ------------------------------------------ */

/* target_path: the in-WC directory to apply the changes into. Pass ""
 * for the WC root (bare "svn merge URL@a:b"). Otherwise something like
 * "src-branch" maps the source-relative paths onto that subdirectory. */
int
svnae_wc_merge(const char *wc_root, const char *source_path, int rev_a, int rev_b,
               const char *target_path)
{
    if (rev_b <= rev_a) return -1;  /* empty or reverse range */
    if (!target_path) target_path = "";

    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    char *base_url = svnae_wc_db_get_info(db, "base_url");
    char *repo     = svnae_wc_db_get_info(db, "repo");
    svnae_wc_db_close(db);
    if (!base_url || !repo) {
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        return -1;
    }

    /* Fetch the source tree at both endpoints. */
    struct rtree A = {0}, B = {0};
    if (walk_remote(base_url, repo, rev_a, source_path, "", &A) != 0 ||
        walk_remote(base_url, repo, rev_b, source_path, "", &B) != 0) {
        rt_free(&A); rt_free(&B);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        return -1;
    }

    /* Compute the diff: for each path in A ∪ B, decide add/mod/del. */
    db = svnae_wc_db_open(wc_root);

    /* Helper: map a source-relative path `p` into the target subdir in
     * the WC, writing into `out` (caller sized PATH_MAX). */
    #define MAP_WC_REL(p, out) do { \
        if (*target_path) snprintf(out, PATH_MAX, "%s/%s", target_path, p); \
        else              snprintf(out, PATH_MAX, "%s", p); \
    } while (0)

    /* Phase 5.13: no pre-scan abort. Per-file, the apply pass decides
     * clean overwrite vs 3-way merge. */

    /* Apply deletions (A has it, B doesn't). */
    for (int i = 0; i < A.n; i++) {
        const struct rnode *ra = &A.items[i];
        if (rt_find(&B, ra->path)) continue;
        char wc_rel[PATH_MAX]; MAP_WC_REL(ra->path, wc_rel);
        char disk[PATH_MAX]; snprintf(disk, sizeof disk, "%s/%s", wc_root, wc_rel);

        if (svnae_wc_db_node_exists(db, wc_rel)) {
            struct svnae_wc_node *n = svnae_wc_db_get_node(db, wc_rel);
            if (n) {
                int brev = svnae_wc_node_base_rev(n);
                const char *bsha = svnae_wc_node_base_sha1(n);
                int kind = svnae_wc_node_kind(n);
                char bsha_copy[41] = {0};
                if (bsha) strncpy(bsha_copy, bsha, 40);
                svnae_wc_node_free(n);
                svnae_wc_db_upsert_node(db, wc_rel, kind, brev, bsha_copy, 2);
            }
            if (ra->kind == 0) unlink(disk); else rmdir(disk);
        }
    }

    /* Apply adds and mods from B. */
    for (int i = 0; i < B.n; i++) {
        const struct rnode *rb = &B.items[i];
        const struct rnode *ra = rt_find(&A, rb->path);

        char wc_rel[PATH_MAX]; MAP_WC_REL(rb->path, wc_rel);
        char disk[PATH_MAX]; snprintf(disk, sizeof disk, "%s/%s", wc_root, wc_rel);

        if (rb->kind == 1) {
            if (mkdir_p(disk) != 0) continue;
            if (!svnae_wc_db_node_exists(db, wc_rel)) {
                svnae_wc_db_upsert_node(db, wc_rel, 1, 0, "", 1);
            }
            continue;
        }

        if (ra && ra->kind == 0 && strcmp(ra->sha1, rb->sha1) == 0) continue;

        /* Does the local file already exist? If yes and it's clean
         * against its pristine, straight overwrite (the change from A
         * to B is safe to apply). If dirty locally, 3-way merge using
         * A as base and B as theirs. If not present locally, straight
         * add. */
        int exists_on_disk = 0;
        {
            struct stat st;
            exists_on_disk = (stat(disk, &st) == 0);
        }
        if (!exists_on_disk || !svnae_wc_db_node_exists(db, wc_rel)) {
            /* Not tracked yet — new add-file. */
            if (write_file_atomic(disk, rb->data, rb->data_len) != 0) continue;
            svnae_wc_pristine_put(wc_root, rb->data, rb->data_len);
            if (!svnae_wc_db_node_exists(db, wc_rel))
                svnae_wc_db_upsert_node(db, wc_rel, 0, 0, "", 1);
            continue;
        }

        /* Tracked. Check for local modification. */
        char disk_sha[41];
        int has_disk_sha = (sha1_of_file(disk, disk_sha) == 0);
        struct svnae_wc_node *n = svnae_wc_db_get_node(db, wc_rel);
        char local_base_sha[41] = {0};
        int  local_base_rev = 0;
        if (n) {
            const char *bs = svnae_wc_node_base_sha1(n);
            if (bs) strncpy(local_base_sha, bs, 40);
            local_base_rev = svnae_wc_node_base_rev(n);
            svnae_wc_node_free(n);
        }
        int local_clean = (has_disk_sha && strcmp(disk_sha, local_base_sha) == 0);

        if (local_clean) {
            /* Safe overwrite — WC file was untouched. */
            if (write_file_atomic(disk, rb->data, rb->data_len) != 0) continue;
            svnae_wc_pristine_put(wc_root, rb->data, rb->data_len);
        } else {
            /* 3-way merge: mine=disk, base=source@A, theirs=source@B. */
            int m3rc = svnae_merge3_apply(disk,
                                          ra ? ra->data : "", ra ? ra->data_len : 0, rev_a,
                                          rb->data, rb->data_len, rev_b);
            if (m3rc == 0) {
                svnae_wc_pristine_put(wc_root, rb->data, rb->data_len);
            } else if (m3rc == 1) {
                svnae_wc_pristine_put(wc_root, rb->data, rb->data_len);
                svnae_wc_db_set_conflicted(db, wc_rel, 1);
                fprintf(stderr, "C    %s\n", wc_rel);
            } else {
                fprintf(stderr, "error merging %s; left alone\n", wc_rel);
            }
            (void)local_base_rev;
        }
    }

    #undef MAP_WC_REL

    svnae_wc_db_close(db);
    rt_free(&A); rt_free(&B);
    svnae_wc_info_free(base_url); svnae_wc_info_free(repo);

    /* Record merged range in svn:mergeinfo on the WC root. */
    mergeinfo_add_range(wc_root, source_path, rev_a, rev_b);
    return 0;
}
