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

extern int svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out);
extern int svnae_wc_hash_file (const char *wc_root, const char *path, char *out);

static __thread const char *g_wc_root = NULL;

static int
sha1_of_file(const char *path, char out[65])
{
    if (!g_wc_root) return -1;
    return svnae_wc_hash_file(g_wc_root, path, out);
}

static void
sha1_of_bytes(const char *data, int len, char out[65])
{
    out[0] = '\0';
    if (g_wc_root) svnae_wc_hash_bytes(g_wc_root, data, len, out);
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

struct rnode { char *path; int kind; char sha1[65]; char *data; int data_len; };
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

/* --- mergeinfo plumbing ---------------------------------------------- *
 *
 * svn:mergeinfo is a line-oriented property. Each line is:
 *   <source-path>:<range>[,<range>...]
 * where <range> is either "A-B" (forward merge of revs A..B inclusive)
 * or "-A-B" (reverse merge of revs A..B inclusive).
 *
 * We canonicalise each source-path's ranges by expanding to per-rev
 * bitmaps (forward_set, reverse_set), cancelling any rev in both
 * (forward+reverse = no-op), then re-serialising as the minimum number
 * of contiguous ranges.
 *
 * The bitmaps are sparse-friendly: we track a sorted int array per set.
 * Revs fit in int because we never go past HEAD of the repo.
 */

#include <ctype.h>

struct revset { int *v; int n, cap; };

static void
rs_add(struct revset *s, int rev)
{
    /* Keep sorted + unique via insertion. O(n) per add but n is tiny
     * in practice (a handful per source). */
    int lo = 0, hi = s->n;
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        if (s->v[m] < rev) lo = m + 1;
        else               hi = m;
    }
    if (lo < s->n && s->v[lo] == rev) return;
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->v = realloc(s->v, (size_t)s->cap * sizeof *s->v);
    }
    memmove(s->v + lo + 1, s->v + lo, (size_t)(s->n - lo) * sizeof *s->v);
    s->v[lo] = rev;
    s->n++;
}

static int
rs_remove(struct revset *s, int rev)
{
    int lo = 0, hi = s->n;
    while (lo < hi) {
        int m = (lo + hi) >> 1;
        if (s->v[m] < rev) lo = m + 1;
        else               hi = m;
    }
    if (lo < s->n && s->v[lo] == rev) {
        memmove(s->v + lo, s->v + lo + 1, (size_t)(s->n - lo - 1) * sizeof *s->v);
        s->n--;
        return 1;
    }
    return 0;
}

static void rs_free(struct revset *s) { free(s->v); s->v = NULL; s->n = s->cap = 0; }

/* Per-source aggregation. */
struct src_entry {
    char         *source;
    struct revset fwd;   /* forward-merged revs   */
    struct revset rev;   /* reverse-merged revs   */
};

struct mi_table {
    struct src_entry *items;
    int               n, cap;
};

static struct src_entry *
mi_find_or_add(struct mi_table *t, const char *source)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->items[i].source, source) == 0) return &t->items[i];
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 8;
        t->items = realloc(t->items, (size_t)t->cap * sizeof *t->items);
    }
    memset(&t->items[t->n], 0, sizeof t->items[t->n]);
    t->items[t->n].source = strdup(source);
    return &t->items[t->n++];
}

static void
mi_free(struct mi_table *t)
{
    for (int i = 0; i < t->n; i++) {
        free(t->items[i].source);
        rs_free(&t->items[i].fwd);
        rs_free(&t->items[i].rev);
    }
    free(t->items);
}

/* Parse one comma-separated range-list like "5-5,7-9,-3-4" into the
 * given forward/reverse revsets. */
static void
parse_ranges_into(const char *list, struct revset *fwd, struct revset *rev)
{
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        int is_reverse = 0;
        if (*p == '-') { is_reverse = 1; p++; }
        if (!isdigit((unsigned char)*p)) { while (*p && *p != ',') p++; continue; }
        int a = 0;
        while (isdigit((unsigned char)*p)) { a = a * 10 + (*p - '0'); p++; }
        int b = a;
        if (*p == '-') {
            p++;
            if (isdigit((unsigned char)*p)) {
                b = 0;
                while (isdigit((unsigned char)*p)) { b = b * 10 + (*p - '0'); p++; }
            }
        }
        struct revset *target = is_reverse ? rev : fwd;
        /* Range is inclusive on both ends. */
        int lo = a < b ? a : b;
        int hi = a < b ? b : a;
        for (int r = lo; r <= hi; r++) rs_add(target, r);
        while (*p && *p != ',') p++;
    }
}

/* Parse the full "source:list\nsource:list\n" blob into the table. */
static void
parse_mergeinfo(const char *text, struct mi_table *out)
{
    if (!text || !*text) return;
    const char *line = text;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        if (llen > 0) {
            const char *colon = memchr(line, ':', llen);
            if (colon) {
                size_t src_len = (size_t)(colon - line);
                char src[512];
                if (src_len < sizeof src) {
                    memcpy(src, line, src_len);
                    src[src_len] = '\0';
                    char list[1024];
                    size_t list_len = llen - src_len - 1;
                    if (list_len < sizeof list) {
                        memcpy(list, colon + 1, list_len);
                        list[list_len] = '\0';
                        struct src_entry *e = mi_find_or_add(out, src);
                        parse_ranges_into(list, &e->fwd, &e->rev);
                    }
                }
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
}

/* After union, cancel revs that appear in both fwd and rev. */
static void
cancel_pairs(struct src_entry *e)
{
    /* Walk both sorted arrays in parallel. */
    int i = 0, j = 0;
    while (i < e->fwd.n && j < e->rev.n) {
        if      (e->fwd.v[i] == e->rev.v[j]) {
            int r = e->fwd.v[i];
            rs_remove(&e->fwd, r);
            rs_remove(&e->rev, r);
            /* indices may have shifted; restart from same i */
        } else if (e->fwd.v[i] < e->rev.v[j]) {
            i++;
        } else {
            j++;
        }
    }
}

/* Serialize a revset as contiguous-range list. Revs are already sorted. */
static void
emit_ranges(const struct revset *s, int is_reverse, char *buf, size_t cap, int *first)
{
    if (s->n == 0) return;
    int i = 0;
    while (i < s->n) {
        int lo = s->v[i];
        int hi = lo;
        while (i + 1 < s->n && s->v[i+1] == hi + 1) { hi++; i++; }
        i++;
        char tmp[64];
        if (is_reverse) snprintf(tmp, sizeof tmp, "-%d-%d", lo, hi);
        else            snprintf(tmp, sizeof tmp, "%d-%d",  lo, hi);
        size_t n = strlen(buf);
        size_t t = strlen(tmp);
        if (n + t + 2 >= cap) return;
        if (!*first) buf[n++] = ',';
        memcpy(buf + n, tmp, t);
        buf[n + t] = '\0';
        *first = 0;
    }
}

/* Render the whole table back to a line-oriented string. Returns
 * malloc'd — caller frees. Empty entries (fwd and rev both empty)
 * are dropped. The table's source order is preserved, which keeps
 * output stable between runs. */
static char *
serialize_mergeinfo(const struct mi_table *t)
{
    size_t cap = 1024;
    char *out = malloc(cap);
    out[0] = '\0';
    for (int i = 0; i < t->n; i++) {
        const struct src_entry *e = &t->items[i];
        if (e->fwd.n == 0 && e->rev.n == 0) continue;
        char line[1024];
        snprintf(line, sizeof line, "%s:", e->source);
        int first = 1;
        emit_ranges(&e->fwd, 0, line, sizeof line, &first);
        emit_ranges(&e->rev, 1, line, sizeof line, &first);
        size_t need = strlen(out) + strlen(line) + 2;
        if (need >= cap) {
            cap = need * 2;
            out = realloc(out, cap);
        }
        if (out[0]) strcat(out, "\n");
        strcat(out, line);
    }
    return out;
}

/* Merge a new range into existing svn:mergeinfo on the WC root,
 * cancelling against reverse entries and collapsing adjacent ones. */
static int
mergeinfo_add_range(const char *wc_root, const char *source,
                    int lo, int hi, int reverse)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    if (!svnae_wc_db_node_exists(db, "")) {
        svnae_wc_db_upsert_node(db, "", 1 /*dir*/, 0, "", 0);
    }
    svnae_wc_db_close(db);

    char *existing = svnae_wc_propget(wc_root, "", "svn:mergeinfo");

    struct mi_table t = {0};
    if (existing && *existing) parse_mergeinfo(existing, &t);
    svnae_wc_props_free(existing);

    struct src_entry *e = mi_find_or_add(&t, source);
    /* Caller's (lo, hi) uses half-open lower bound: forward-merge of
     * rev-range (lo, hi] means revs lo+1..hi inclusive. Reverse-merge
     * of (lo, hi] (as recorded by svnae_wc_merge) also covers revs
     * lo+1..hi inclusive. So both kinds span the same rev numbers.
     * Expand into the appropriate set. */
    struct revset *set = reverse ? &e->rev : &e->fwd;
    for (int r = lo + 1; r <= hi; r++) rs_add(set, r);

    cancel_pairs(e);

    char *new_val = serialize_mergeinfo(&t);
    mi_free(&t);

    int rc;
    if (new_val && *new_val) {
        rc = svnae_wc_propset(wc_root, "", "svn:mergeinfo", new_val);
    } else {
        /* Empty result — delete the property entirely. */
        extern int svnae_wc_propdel(const char *wc_root, const char *path, const char *name);
        svnae_wc_propdel(wc_root, "", "svn:mergeinfo");
        rc = 0;
    }
    free(new_val);
    return rc;
}

/* --- main merge entry point ------------------------------------------ */

/* target_path: the in-WC directory to apply the changes into. Pass ""
 * for the WC root (bare "svn merge URL@a:b"). Otherwise something like
 * "src-branch" maps the source-relative paths onto that subdirectory.
 *
 * rev_a < rev_b:  forward merge — apply (source@A → source@B) onto WC.
 * rev_a > rev_b:  reverse merge — undo the changes introduced by
 *                 (source@rev_b → source@rev_a). Internally we swap
 *                 the tree endpoints so the apply pass always diffs
 *                 from "base" to "theirs"; the diff direction is what
 *                 makes forward vs reverse happen.
 * rev_a == rev_b: no-op, returns 0.
 */
int
svnae_wc_merge(const char *wc_root, const char *source_path, int rev_a, int rev_b,
               const char *target_path)
{
    g_wc_root = wc_root;
    if (rev_a == rev_b) return 0;
    if (!target_path) target_path = "";

    int reverse  = (rev_a > rev_b);
    /* After the swap, rev_base < rev_theirs always; the original
     * rev_a/rev_b are preserved only for mergeinfo bookkeeping. */
    int rev_base   = reverse ? rev_b : rev_a;
    int rev_theirs = reverse ? rev_a : rev_b;

    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    char *base_url = svnae_wc_db_get_info(db, "base_url");
    char *repo     = svnae_wc_db_get_info(db, "repo");
    svnae_wc_db_close(db);
    if (!base_url || !repo) {
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        return -1;
    }

    /* Fetch the source tree at both endpoints. A is the "base" tree
     * (what gets diffed away); B is the "theirs" tree (what gets
     * applied). For a forward merge A=rev_a, B=rev_b. For reverse
     * merge A=rev_b, B=rev_a — so undoing a change means the pre-change
     * tree is "theirs" and replaces the post-change tree. */
    struct rtree A = {0}, B = {0};
    if (walk_remote(base_url, repo, rev_base,   source_path, "", &A) != 0 ||
        walk_remote(base_url, repo, rev_theirs, source_path, "", &B) != 0) {
        rt_free(&A); rt_free(&B);
        svnae_wc_info_free(base_url); svnae_wc_info_free(repo);
        return -1;
    }
    if (reverse) {
        /* Swap so the apply pass below uses (base=A=post-change,
         * theirs=B=pre-change). The semantics are: file that exists
         * in "theirs" but not in "base" is ADDED to the WC (undoing a
         * server-side delete); file that exists in "base" but not
         * in "theirs" is REMOVED from the WC (undoing a server-side
         * add); file that differs between them is text-merged with
         * the pre-change content as the incoming text. */
        struct rtree tmp = A; A = B; B = tmp;
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
                char bsha_copy[65] = {0};
                if (bsha) {
                    size_t l = strlen(bsha);
                    if (l >= sizeof bsha_copy) l = sizeof bsha_copy - 1;
                    memcpy(bsha_copy, bsha, l);
                    bsha_copy[l] = '\0';
                }
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
        char disk_sha[65];
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
        (void)has_disk_sha;
        (void)local_base_rev;
        /* Always use 3-way merge: mine=disk, base=source@A,
         * theirs=source@B. For forward merges where the WC is clean
         * this collapses to a straight overwrite anyway (no
         * conflicting region). For reverse merges and cherry-picks
         * the WC is NOT synced to A or B, so we must let merge3
         * splice the base→theirs diff into mine rather than
         * blindly overwriting. */
        int m3rc = svnae_merge3_apply(disk,
                                      ra ? ra->data : "", ra ? ra->data_len : 0, rev_a,
                                      rb->data, rb->data_len, rev_b);
        if (m3rc == 0) {
            /* Clean merge. We do NOT update the pristine here because
             * the WC's base is still the WC's original head — we only
             * spliced an external rev-range diff into `mine`. Commit
             * will pick up the change via disk-vs-pristine status. */
        } else if (m3rc == 1) {
            svnae_wc_db_set_conflicted(db, wc_rel, 1);
            fprintf(stderr, "C    %s\n", wc_rel);
        } else {
            fprintf(stderr, "error merging %s; left alone\n", wc_rel);
        }
        (void)local_base_sha;
    }

    #undef MAP_WC_REL

    svnae_wc_db_close(db);
    rt_free(&A); rt_free(&B);
    svnae_wc_info_free(base_url); svnae_wc_info_free(repo);

    /* Record merged range in svn:mergeinfo on the WC root. We always
     * store as (lo, hi) with a reverse flag — the line emitted gets
     * the leading '-' when reverse == 1. */
    mergeinfo_add_range(wc_root, source_path, rev_base, rev_theirs, reverse);
    return 0;
}
