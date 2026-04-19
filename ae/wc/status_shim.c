/* ae/wc/status_shim.c — svn status.
 *
 * Compares the on-disk tree under $wc against what wc.db thinks is there
 * plus the pristine store. Produces a list of status entries:
 *
 *   path, status_code
 *     ' '  Normal (tracked, unchanged)                — filtered from output
 *     'A'  Added (scheduled for commit, no base)
 *     'D'  Deleted (scheduled for commit, was tracked)
 *     'M'  Modified (tracked, disk bytes differ from pristine)
 *     '!'  Missing (tracked, gone from disk)
 *     '?'  Unversioned (on disk, not tracked, not .svn/)
 *
 * Algorithm:
 *   1. Walk wc.db rows. For each:
 *        - kind=file, state=normal: read disk file, sha1 it,
 *          compare to base_sha1 → Normal or Modified; if disk missing → Missing.
 *        - kind=file, state=added:    'A'
 *        - kind=file, state=deleted:  'D'
 *        - kind=dir,  state=normal: ensure dir exists; Missing if not.
 *   2. Walk the filesystem. For each file/dir not in the db and not under
 *      .svn/, emit '?'.
 *
 * We only return changed entries in the API (filter Normal here). Output
 * order is lexicographic path.
 */

#include <dirent.h>
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

/* Externs from neighbouring shims. */
sqlite3     *svnae_wc_db_open(const char *wc_root);
void         svnae_wc_db_close(sqlite3 *db);

struct svnae_wc_nodelist;
struct svnae_wc_nodelist *svnae_wc_db_list_nodes(sqlite3 *db);
int         svnae_wc_nodelist_count(const struct svnae_wc_nodelist *L);
const char *svnae_wc_nodelist_path(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_kind(const struct svnae_wc_nodelist *L, int i);
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_state(const struct svnae_wc_nodelist *L, int i);
void        svnae_wc_nodelist_free(struct svnae_wc_nodelist *L);

/* ---- helpers ---------------------------------------------------------- */

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

/* ---- result list ----------------------------------------------------- */

struct status_entry { char *path; char code; };
struct svnae_wc_statuslist { struct status_entry *items; int n; int cap; };

static void
add_entry(struct svnae_wc_statuslist *L, const char *path, char code)
{
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 8;
        L->items = realloc(L->items, (size_t)nc * sizeof *L->items);
        L->cap = nc;
    }
    L->items[L->n].path = strdup(path);
    L->items[L->n].code = code;
    L->n++;
}

static int
entry_cmp(const void *a, const void *b)
{
    return strcmp(((const struct status_entry *)a)->path,
                  ((const struct status_entry *)b)->path);
}

/* ---- filesystem walk for unversioned ---------------------------------- *
 *
 * Walks $wc on disk, skipping .svn/, and emits '?' for any path not
 * present in `tracked` (a simple strset of known paths from wc.db).
 */

struct strset { char **items; int n; int cap; };

static int
strset_has(const struct strset *s, const char *v)
{
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->items[i], v) == 0) return 1;
    return 0;
}

static void
strset_add(struct strset *s, const char *v)
{
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        s->items = realloc(s->items, (size_t)nc * sizeof *s->items);
        s->cap = nc;
    }
    s->items[s->n++] = strdup(v);
}

static void
strset_clear(struct strset *s)
{
    for (int i = 0; i < s->n; i++) free(s->items[i]);
    free(s->items);
    s->items = NULL; s->n = 0; s->cap = 0;
}

static void
walk_unversioned(const char *wc_root, const char *rel,
                 const struct strset *tracked,
                 struct svnae_wc_statuslist *out)
{
    char dir_path[PATH_MAX];
    if (*rel) snprintf(dir_path, sizeof dir_path, "%s/%s", wc_root, rel);
    else      snprintf(dir_path, sizeof dir_path, "%s", wc_root);

    DIR *d = opendir(dir_path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0'
            || (e->d_name[1] == '.' && e->d_name[2] == '\0'))) continue;
        if (strcmp(e->d_name, ".svn") == 0) continue;

        char child_rel[PATH_MAX];
        if (*rel) snprintf(child_rel, sizeof child_rel, "%s/%s", rel, e->d_name);
        else      snprintf(child_rel, sizeof child_rel, "%s", e->d_name);

        char child_fs[PATH_MAX];
        snprintf(child_fs, sizeof child_fs, "%s/%s", wc_root, child_rel);
        struct stat st;
        if (lstat(child_fs, &st) != 0) continue;

        int is_tracked = strset_has(tracked, child_rel);
        if (S_ISDIR(st.st_mode)) {
            if (!is_tracked) {
                add_entry(out, child_rel, '?');
                /* Don't recurse into unversioned dirs — reference svn
                 * reports the dir once and stops there. */
            } else {
                walk_unversioned(wc_root, child_rel, tracked, out);
            }
        } else if (S_ISREG(st.st_mode)) {
            if (!is_tracked) add_entry(out, child_rel, '?');
        }
    }
    closedir(d);
}

/* ---- public API ------------------------------------------------------ */

struct svnae_wc_statuslist *
svnae_wc_status(const char *wc_root)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return NULL;

    struct svnae_wc_statuslist *out = calloc(1, sizeof *out);
    struct strset tracked = {0};

    /* Pass 1: every tracked node. */
    struct svnae_wc_nodelist *L = svnae_wc_db_list_nodes(db);
    int n = svnae_wc_nodelist_count(L);
    for (int i = 0; i < n; i++) {
        const char *rel = svnae_wc_nodelist_path(L, i);
        int kind  = svnae_wc_nodelist_kind(L, i);
        int state = svnae_wc_nodelist_state(L, i);
        const char *base_sha = svnae_wc_nodelist_base_sha1(L, i);

        strset_add(&tracked, rel);

        char disk[PATH_MAX];
        snprintf(disk, sizeof disk, "%s/%s", wc_root, rel);

        struct stat st;
        int on_disk = (lstat(disk, &st) == 0);

        if (state == 1 /*added*/) { add_entry(out, rel, 'A'); continue; }
        if (state == 2 /*deleted*/) { add_entry(out, rel, 'D'); continue; }
        /* Normal: compare. */
        if (!on_disk) { add_entry(out, rel, '!'); continue; }
        if (kind == 1 /*dir*/) {
            /* Tracked directory that exists — nothing to say. */
            continue;
        }
        /* Tracked file: hash it. */
        char cur[41];
        if (sha1_of_file(disk, cur) != 0) { add_entry(out, rel, '!'); continue; }
        if (strcmp(cur, base_sha) != 0) add_entry(out, rel, 'M');
    }
    svnae_wc_nodelist_free(L);

    /* Pass 2: filesystem walk for '?' entries. */
    walk_unversioned(wc_root, "", &tracked, out);

    strset_clear(&tracked);
    svnae_wc_db_close(db);

    qsort(out->items, (size_t)out->n, sizeof *out->items, entry_cmp);
    return out;
}

int         svnae_wc_statuslist_count(const struct svnae_wc_statuslist *L) { return L ? L->n : 0; }

const char *
svnae_wc_statuslist_path(const struct svnae_wc_statuslist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].path;
}

int
svnae_wc_statuslist_code(const struct svnae_wc_statuslist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return 0;
    return (int)(unsigned char)L->items[i].code;
}

void
svnae_wc_statuslist_free(struct svnae_wc_statuslist *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) free(L->items[i].path);
    free(L->items);
    free(L);
}
