/* ae/wc/commit_shim.c — wc-backed commit.
 *
 * Packages the WC's pending changes into an RA commit:
 *   1. Walk wc.db for rows with state != 0 (added, deleted, replaced).
 *      Also walk state=0 file rows and compare their on-disk sha1
 *      against base_sha1 → if different, treat as modified (→ add-file
 *      edit in the txn).
 *   2. Read info kv to get base_url, repo_name, base_rev.
 *   3. Build an RA commit via the Phase 7 API, POST it.
 *   4. On success, rewrite node rows: state=0, base_rev=new_rev,
 *      base_sha1=current content sha1 for added/modified, delete rows
 *      for deleted.
 *   5. Also refresh the pristine store with the newly-committed bytes.
 *
 * API:
 *   svnae_wc_commit(wc_root, author, logmsg) -> new_rev | -1
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

/* --- externs --------------------------------------------------------- */

sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_upsert_node(sqlite3 *db, const char *path, int kind, int base_rev, const char *sha1, int state);
int      svnae_wc_db_delete_node(sqlite3 *db, const char *path);

struct svnae_wc_nodelist;
struct svnae_wc_nodelist *svnae_wc_db_list_nodes(sqlite3 *db);
int         svnae_wc_nodelist_count(const struct svnae_wc_nodelist *L);
const char *svnae_wc_nodelist_path(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_kind(const struct svnae_wc_nodelist *L, int i);
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_state(const struct svnae_wc_nodelist *L, int i);
void        svnae_wc_nodelist_free(struct svnae_wc_nodelist *L);

char *svnae_wc_db_get_info(sqlite3 *db, const char *key);
int   svnae_wc_db_set_info(sqlite3 *db, const char *key, const char *value);
void  svnae_wc_info_free(char *s);

const char *svnae_wc_pristine_put(const char *wc_root, const char *data, int len);

struct svnae_ra_commit;
struct svnae_ra_commit *svnae_ra_commit_begin(int base_rev, const char *author, const char *logmsg);
int  svnae_ra_commit_add_file(struct svnae_ra_commit *cb, const char *path, const char *content, int len);
int  svnae_ra_commit_mkdir(struct svnae_ra_commit *cb, const char *path);
int  svnae_ra_commit_delete(struct svnae_ra_commit *cb, const char *path);
int  svnae_ra_commit_finish(struct svnae_ra_commit *cb, const char *base_url, const char *repo_name);

/* --- helpers --------------------------------------------------------- */

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
    unsigned char dig[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
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

static char *
read_file_to_malloc(const char *path, int *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t got = 0;
    while (got < st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)(st.st_size - got));
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (n == 0) break;
        got += n;
    }
    close(fd);
    buf[got] = '\0';
    *out_len = (int)got;
    return buf;
}

/* --- main entry point ------------------------------------------------ */

int
svnae_wc_commit(const char *wc_root, const char *author, const char *logmsg)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;

    char *base_url = svnae_wc_db_get_info(db, "base_url");
    char *repo     = svnae_wc_db_get_info(db, "repo");
    char *base_rev_s = svnae_wc_db_get_info(db, "base_rev");
    if (!base_url || !repo || !base_rev_s) {
        svnae_wc_db_close(db);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo); svnae_wc_info_free(base_rev_s);
        return -1;
    }
    int base_rev = atoi(base_rev_s);
    svnae_wc_info_free(base_rev_s);

    /* Gather pending edits from the WC. We collect into parallel arrays
     * so we can redo the db writes after a successful commit. */
    struct svnae_wc_nodelist *L = svnae_wc_db_list_nodes(db);
    int n = svnae_wc_nodelist_count(L);

    /* Two-pass: first build the RA commit, then if it succeeds, walk the
     * same list again and update rows. We keep the rows' roles in a
     * small per-node int array:
     *   0 = no edit
     *   1 = add-file (new file or modified)
     *   2 = mkdir
     *   3 = delete
     */
    int *roles = calloc((size_t)n, sizeof *roles);
    char **new_sha1s = calloc((size_t)n, sizeof *new_sha1s);   /* for role=1 files */

    struct svnae_ra_commit *cb = svnae_ra_commit_begin(base_rev, author, logmsg);
    int any_edits = 0;

    for (int i = 0; i < n; i++) {
        const char *rel   = svnae_wc_nodelist_path(L, i);
        int kind          = svnae_wc_nodelist_kind(L, i);
        int state         = svnae_wc_nodelist_state(L, i);
        const char *base_sha = svnae_wc_nodelist_base_sha1(L, i);

        char disk[PATH_MAX];
        snprintf(disk, sizeof disk, "%s/%s", wc_root, rel);

        if (state == 2 /*deleted*/) {
            svnae_ra_commit_delete(cb, rel);
            roles[i] = 3;
            any_edits = 1;
            continue;
        }
        if (state == 1 /*added*/) {
            if (kind == 1 /*dir*/) {
                svnae_ra_commit_mkdir(cb, rel);
                roles[i] = 2;
                any_edits = 1;
                continue;
            }
            /* added file — read content. */
            int len = 0;
            char *data = read_file_to_malloc(disk, &len);
            if (!data) continue;
            svnae_ra_commit_add_file(cb, rel, data, len);
            /* Record sha1 for post-commit pristine + db refresh. */
            char sha[41];
            EVP_MD_CTX *ctx = EVP_MD_CTX_new();
            unsigned char dig[EVP_MAX_MD_SIZE]; unsigned int dlen = 0;
            EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
            EVP_DigestUpdate(ctx, data, (size_t)len);
            EVP_DigestFinal_ex(ctx, dig, &dlen);
            EVP_MD_CTX_free(ctx);
            static const char hex[] = "0123456789abcdef";
            for (unsigned int j = 0; j < 20; j++) {
                sha[j*2]   = hex[dig[j] >> 4];
                sha[j*2+1] = hex[dig[j] & 0xf];
            }
            sha[40] = '\0';
            new_sha1s[i] = strdup(sha);
            svnae_wc_pristine_put(wc_root, data, len);
            free(data);
            roles[i] = 1;
            any_edits = 1;
            continue;
        }
        /* state == normal. For files: modified? */
        if (kind == 0 /*file*/) {
            char disk_sha[41];
            if (sha1_of_file(disk, disk_sha) != 0) continue;
            if (strcmp(disk_sha, base_sha) == 0) continue;  /* unchanged */
            /* Modified — same wire shape as add-file. */
            int len = 0;
            char *data = read_file_to_malloc(disk, &len);
            if (!data) continue;
            svnae_ra_commit_add_file(cb, rel, data, len);
            new_sha1s[i] = strdup(disk_sha);
            svnae_wc_pristine_put(wc_root, data, len);
            free(data);
            roles[i] = 1;
            any_edits = 1;
        }
    }

    if (!any_edits) {
        /* Nothing to commit. Clean up builder by calling finish anyway;
         * with an empty edit list the server still produces a new rev
         * (matching our HTTP-layer behaviour). For tests we want a
         * meaningful return value, so treat this as "no op" == -2. */
        svnae_ra_commit_finish(cb, base_url, repo);
        svnae_wc_nodelist_free(L);
        svnae_wc_db_close(db);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        for (int i = 0; i < n; i++) free(new_sha1s[i]);
        free(roles); free(new_sha1s);
        return -2;
    }

    int new_rev = svnae_ra_commit_finish(cb, base_url, repo);
    if (new_rev < 0) {
        svnae_wc_nodelist_free(L);
        svnae_wc_db_close(db);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        for (int i = 0; i < n; i++) free(new_sha1s[i]);
        free(roles); free(new_sha1s);
        return -1;
    }

    /* Success — reconcile wc.db with the new base revision. */
    for (int i = 0; i < n; i++) {
        const char *rel = svnae_wc_nodelist_path(L, i);
        int kind        = svnae_wc_nodelist_kind(L, i);
        int role = roles[i];
        if (role == 3) {
            svnae_wc_db_delete_node(db, rel);
        } else if (role == 2) {
            svnae_wc_db_upsert_node(db, rel, 1 /*dir*/, new_rev, "", 0);
        } else if (role == 1) {
            svnae_wc_db_upsert_node(db, rel, 0 /*file*/, new_rev, new_sha1s[i], 0);
        } else {
            /* Unchanged node — still bump base_rev to the new rev so
             * subsequent commits have a consistent base. */
            svnae_wc_db_upsert_node(db, rel, kind, new_rev,
                                    svnae_wc_nodelist_base_sha1(L, i), 0);
        }
    }

    /* Update base_rev in info. */
    char buf[16]; snprintf(buf, sizeof buf, "%d", new_rev);
    svnae_wc_db_set_info(db, "base_rev", buf);

    svnae_wc_nodelist_free(L);
    svnae_wc_db_close(db);
    svnae_wc_info_free(base_url);
    svnae_wc_info_free(repo);
    for (int i = 0; i < n; i++) free(new_sha1s[i]);
    free(roles); free(new_sha1s);
    return new_rev;
}
