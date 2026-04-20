/* repos/shim.c — libsvn_repos query surface.
 *
 * Thin layer on top of fs_fs. Everything here reads the repo on-disk
 * format we built in Phase 3.x: $repo/head + $repo/revs/NNNNNN + rep-cache.db.
 *
 * Three public query operations for this phase:
 *   svnae_repos_log        — enumerate revisions with metadata
 *   svnae_repos_cat        — read file content at (rev, path)
 *   svnae_repos_list       — list entries of a directory at (rev, path)
 *   svnae_repos_info_rev   — metadata for a single revision
 *
 * The Aether side receives results via small opaque handles (already the
 * port's dominant idiom) and walks them with index-based accessors.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* We piggyback on the rep-store shim for blob reads and on the fs_fs shim
 * for small-file reads. Declarations here; definitions link in from the
 * other shims (see aether.toml extra_sources for the test binary). */

char *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
void  svnae_rep_free(char *p);

/* --- helpers --------------------------------------------------------- */

/* Slurp an entire small file (head, revs/NNNNNN pointer). Returns
 * malloc'd NUL-terminated bytes or NULL. Caller frees. */
static char *
read_small(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size;
    char *buf = malloc(size + 1);
    if (!buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != size) { free(buf); return NULL; }
    buf[size] = '\0';
    return buf;
}

static void
trim_trailing_newline(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) { s[--n] = '\0'; }
}

static int
parse_int(const char *s, int *out)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s) return 0;
    *out = (int)v;
    return 1;
}

static int
rev_pointer_path(const char *repo, int rev, char *out, size_t out_sz)
{
    return snprintf(out, out_sz, "%s/revs/%06d", repo, rev);
}

static int
head_rev(const char *repo)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/head", repo);
    char *body = read_small(path);
    if (!body) return -1;
    trim_trailing_newline(body);
    int v = -1;
    parse_int(body, &v);
    free(body);
    return v;
}

/* Return the revision blob's SHA-1 for rev `n`, as a malloc'd string.
 * NULL on failure. */
static char *
rev_blob_sha1(const char *repo, int rev)
{
    char path[PATH_MAX];
    rev_pointer_path(repo, rev, path, sizeof path);
    char *body = read_small(path);
    if (!body) return NULL;
    trim_trailing_newline(body);
    return body;
}

/* Extract "key: value" from a line-oriented blob. Returns malloc'd value
 * or NULL if the key isn't present. */
static char *
parse_field(const char *body, const char *key)
{
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *eol = strchr(v, '\n');
            size_t n = eol ? (size_t)(eol - v) : strlen(v);
            char *out = malloc(n + 1);
            memcpy(out, v, n);
            out[n] = '\0';
            return out;
        }
        const char *eol = strchr(p, '\n');
        if (!eol) break;
        p = eol + 1;
    }
    return NULL;
}

/* --- log ---------------------------------------------------------------
 *
 * The commit order in the repo is simply 0..HEAD, which is the natural
 * "historical" order. For now we expose log as an array of entries
 * indexed by revision number. Path filtering and copy tracing come later
 * when the tree-diff machinery lands.
 */

struct log_entry {
    int   rev;
    char *author;
    char *date;
    char *msg;
};

struct svnae_log {
    struct log_entry *entries;
    int n;
};

/* Build a full log from rev 0 to HEAD. Returns NULL on error. */
struct svnae_log *
svnae_repos_log(const char *repo)
{
    int head = head_rev(repo);
    if (head < 0) return NULL;

    struct svnae_log *lg = calloc(1, sizeof *lg);
    if (!lg) return NULL;
    lg->n = head + 1;
    lg->entries = calloc((size_t)lg->n, sizeof *lg->entries);
    if (!lg->entries) { free(lg); return NULL; }

    for (int r = 0; r <= head; r++) {
        char *sha1 = rev_blob_sha1(repo, r);
        if (!sha1) continue;
        char *body = svnae_rep_read_blob(repo, sha1);
        free(sha1);
        if (!body) continue;

        lg->entries[r].rev    = r;
        lg->entries[r].author = parse_field(body, "author");
        lg->entries[r].date   = parse_field(body, "date");
        lg->entries[r].msg    = parse_field(body, "log");
        svnae_rep_free(body);
    }
    return lg;
}

int svnae_repos_log_count(const struct svnae_log *lg) { return lg ? lg->n : 0; }

int
svnae_repos_log_rev(const struct svnae_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n) return -1;
    return lg->entries[i].rev;
}

const char *
svnae_repos_log_author(const struct svnae_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n || !lg->entries[i].author) return "";
    return lg->entries[i].author;
}

const char *
svnae_repos_log_date(const struct svnae_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n || !lg->entries[i].date) return "";
    return lg->entries[i].date;
}

const char *
svnae_repos_log_msg(const struct svnae_log *lg, int i)
{
    if (!lg || i < 0 || i >= lg->n || !lg->entries[i].msg) return "";
    return lg->entries[i].msg;
}

void
svnae_repos_log_free(struct svnae_log *lg)
{
    if (!lg) return;
    for (int i = 0; i < lg->n; i++) {
        free(lg->entries[i].author);
        free(lg->entries[i].date);
        free(lg->entries[i].msg);
    }
    free(lg->entries);
    free(lg);
}

/* --- cat / list: tree walk ---------------------------------------------
 *
 * Same directory blob format as Phase 3.3 (one line per entry,
 * "<kind> <sha1> <name>\n"). For cat we walk to the file entry and
 * read its blob. For list we read the directory blob and parse entries.
 */

static char *
root_dir_sha1_for_rev(const char *repo, int rev)
{
    char *sha1 = rev_blob_sha1(repo, rev);
    if (!sha1) return NULL;
    char *body = svnae_rep_read_blob(repo, sha1);
    free(sha1);
    if (!body) return NULL;
    char *root = parse_field(body, "root");
    svnae_rep_free(body);
    return root;
}

/* Resolve a path against a dir-body chain. Returns {kind_char, sha1} on
 * success. `out_sha1` is a malloc'd 40-char hex string the caller frees.
 * `out_kind` is 'f', 'd', or 0 on miss. */
static int
resolve_path(const char *repo, const char *root_sha1, const char *path,
             char *out_kind, char **out_sha1)
{
    *out_kind = 0;
    *out_sha1 = NULL;

    /* Skip leading '/' and any trailing. */
    const char *p = path;
    while (*p == '/') p++;

    if (!*p) {
        /* Path is the root itself. */
        *out_kind = 'd';
        *out_sha1 = strdup(root_sha1);
        return 1;
    }

    char cur_sha1[41];
    memcpy(cur_sha1, root_sha1, 40);
    cur_sha1[40] = '\0';
    char *cur_body = svnae_rep_read_blob(repo, cur_sha1);
    if (!cur_body) return 0;

    while (*p) {
        /* Extract the next segment. */
        const char *seg_start = p;
        while (*p && *p != '/') p++;
        size_t seg_len = (size_t)(p - seg_start);
        char seg[256];
        if (seg_len >= sizeof seg) { svnae_rep_free(cur_body); return 0; }
        memcpy(seg, seg_start, seg_len);
        seg[seg_len] = '\0';

        int at_end = (*p == '\0');
        if (*p == '/') p++;

        /* Walk cur_body lines looking for `seg`. */
        char *lp = cur_body;
        char found_kind = 0;
        char found_sha1[41] = {0};
        while (*lp) {
            char *eol = strchr(lp, '\n');
            size_t llen = eol ? (size_t)(eol - lp) : strlen(lp);
            if (llen >= 44) {
                /* Kind at [0], sha1 at [2..42], name at [43..] */
                size_t name_len = llen - 43;
                if (name_len == seg_len && memcmp(lp + 43, seg, seg_len) == 0) {
                    found_kind = lp[0];
                    memcpy(found_sha1, lp + 2, 40);
                    found_sha1[40] = '\0';
                    break;
                }
            }
            if (!eol) break;
            lp = eol + 1;
        }
        svnae_rep_free(cur_body);
        cur_body = NULL;
        if (!found_kind) return 0;

        if (at_end) {
            *out_kind = found_kind;
            *out_sha1 = strdup(found_sha1);
            return 1;
        }
        /* Must be a directory to keep walking. */
        if (found_kind != 'd') return 0;
        memcpy(cur_sha1, found_sha1, 40);
        cur_sha1[40] = '\0';
        cur_body = svnae_rep_read_blob(repo, cur_sha1);
        if (!cur_body) return 0;
    }

    /* Shouldn't reach here. */
    if (cur_body) svnae_rep_free(cur_body);
    return 0;
}

/* Read file content at (rev, path). Returns malloc'd bytes or NULL.
 * Caller frees with svnae_rep_free. */
char *
svnae_repos_cat(const char *repo, int rev, const char *path)
{
    char *root = root_dir_sha1_for_rev(repo, rev);
    if (!root) return NULL;
    char kind = 0;
    char *sha1 = NULL;
    int ok = resolve_path(repo, root, path, &kind, &sha1);
    free(root);
    if (!ok || kind != 'f') { free(sha1); return NULL; }
    char *data = svnae_rep_read_blob(repo, sha1);
    free(sha1);
    return data;
}

/* --- list ---------------------------------------------------------------
 *
 * svnae_repos_list(repo, rev, path) → handle containing entries.
 * Each entry: name + kind ('f' or 'd'). We deliberately don't surface the
 * content sha1 — that's an fs_fs-internal concern clients shouldn't know
 * about.
 */

struct list_entry { char *name; char kind; };
struct svnae_list { struct list_entry *items; int n; };

struct svnae_list *
svnae_repos_list(const char *repo, int rev, const char *path)
{
    char *root = root_dir_sha1_for_rev(repo, rev);
    if (!root) return NULL;
    char kind = 0;
    char *dir_sha1 = NULL;
    int ok = resolve_path(repo, root, path, &kind, &dir_sha1);
    free(root);
    if (!ok || kind != 'd') { free(dir_sha1); return NULL; }

    char *body = svnae_rep_read_blob(repo, dir_sha1);
    free(dir_sha1);
    if (!body) return NULL;

    struct svnae_list *L = calloc(1, sizeof *L);
    /* Count lines first for allocation. */
    int lines = 0;
    for (char *lp = body; *lp; ) {
        char *eol = strchr(lp, '\n');
        if (!eol) { if (*lp) lines++; break; }
        lines++;
        lp = eol + 1;
    }
    L->items = calloc((size_t)lines, sizeof *L->items);

    int i = 0;
    for (char *lp = body; *lp; ) {
        char *eol = strchr(lp, '\n');
        size_t llen = eol ? (size_t)(eol - lp) : strlen(lp);
        if (llen >= 44) {
            L->items[i].kind = lp[0];
            size_t name_len = llen - 43;
            L->items[i].name = malloc(name_len + 1);
            memcpy(L->items[i].name, lp + 43, name_len);
            L->items[i].name[name_len] = '\0';
            i++;
        }
        if (!eol) break;
        lp = eol + 1;
    }
    L->n = i;
    svnae_rep_free(body);
    return L;
}

int svnae_repos_list_count(const struct svnae_list *L) { return L ? L->n : 0; }

const char *
svnae_repos_list_name(const struct svnae_list *L, int i)
{
    if (!L || i < 0 || i >= L->n || !L->items[i].name) return "";
    return L->items[i].name;
}

const char *
svnae_repos_list_kind(const struct svnae_list *L, int i)
{
    static const char f[] = "file";
    static const char d[] = "dir";
    static const char u[] = "";
    if (!L || i < 0 || i >= L->n) return u;
    return L->items[i].kind == 'f' ? f : (L->items[i].kind == 'd' ? d : u);
}

void
svnae_repos_list_free(struct svnae_list *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) free(L->items[i].name);
    free(L->items);
    free(L);
}

/* --- info (single-revision metadata) --------------------------------- *
 *
 * A convenience for when you know a specific rev# and don't want to
 * walk the whole log. Returns a tiny struct accessed with the same
 * accessor shape as log entries. */

struct svnae_info {
    int   rev;
    char *author;
    char *date;
    char *msg;
};

struct svnae_info *
svnae_repos_info_rev(const char *repo, int rev)
{
    char *sha1 = rev_blob_sha1(repo, rev);
    if (!sha1) return NULL;
    char *body = svnae_rep_read_blob(repo, sha1);
    free(sha1);
    if (!body) return NULL;
    struct svnae_info *I = calloc(1, sizeof *I);
    I->rev    = rev;
    I->author = parse_field(body, "author");
    I->date   = parse_field(body, "date");
    I->msg    = parse_field(body, "log");
    svnae_rep_free(body);
    return I;
}

int         svnae_repos_info_rev_num(const struct svnae_info *I) { return I ? I->rev : -1; }
const char *svnae_repos_info_author (const struct svnae_info *I) { return I && I->author ? I->author : ""; }
const char *svnae_repos_info_date   (const struct svnae_info *I) { return I && I->date   ? I->date   : ""; }
const char *svnae_repos_info_msg    (const struct svnae_info *I) { return I && I->msg    ? I->msg    : ""; }

void
svnae_repos_info_free(struct svnae_info *I)
{
    if (!I) return;
    free(I->author); free(I->date); free(I->msg);
    free(I);
}

int svnae_repos_head_rev(const char *repo) { return head_rev(repo); }

/* --- paths changed in a single revision -----------------------------
 *
 * For `svn log --verbose`: given rev N we want the list of paths added,
 * modified, or deleted in N relative to N-1. Both trees get flattened
 * into a sorted (path → (kind, sha1)) table by recursively walking
 * the dir blobs; then a linear merge classifies each path:
 *   present in both, same sha1 → skip
 *   present in both, different sha1 → 'M'
 *   only in N     → 'A'
 *   only in N-1   → 'D'
 *
 * Directories are included so that purely-empty dir adds/deletes show
 * up; their sha1 is whatever their dir-blob sha hashes to, which changes
 * whenever any descendant does, so subdirs that are structurally the
 * same across revs hash identically and get skipped. That's a happy
 * rep-sharing side effect. */

struct flat_entry { char *path; char kind; char sha1[41]; };
struct flat_tree  { struct flat_entry *items; int n, cap; };

static void
flat_add(struct flat_tree *t, const char *path, char kind, const char *sha1)
{
    if (t->n == t->cap) {
        int nc = t->cap ? t->cap * 2 : 64;
        t->items = realloc(t->items, (size_t)nc * sizeof *t->items);
        t->cap = nc;
    }
    t->items[t->n].path = strdup(path);
    t->items[t->n].kind = kind;
    memcpy(t->items[t->n].sha1, sha1, 40);
    t->items[t->n].sha1[40] = '\0';
    t->n++;
}

static void
flat_free(struct flat_tree *t)
{
    for (int i = 0; i < t->n; i++) free(t->items[i].path);
    free(t->items);
}

/* Depth-first walk of a dir-blob, emitting "<prefix>/<name>" entries
 * for every file and directory reachable from `dir_sha1`. */
static void
flatten_tree(const char *repo, const char *dir_sha1, const char *prefix,
             struct flat_tree *out)
{
    char *body = svnae_rep_read_blob(repo, dir_sha1);
    if (!body) return;
    char *lp = body;
    while (*lp) {
        char *eol = strchr(lp, '\n');
        size_t llen = eol ? (size_t)(eol - lp) : strlen(lp);
        if (llen >= 44) {
            char kind = lp[0];
            char child_sha[41];
            memcpy(child_sha, lp + 2, 40);
            child_sha[40] = '\0';
            size_t name_len = llen - 43;
            char name[512];
            if (name_len < sizeof name) {
                memcpy(name, lp + 43, name_len);
                name[name_len] = '\0';

                char child_path[PATH_MAX];
                if (*prefix) snprintf(child_path, sizeof child_path, "%s/%s", prefix, name);
                else         snprintf(child_path, sizeof child_path, "%s",    name);

                flat_add(out, child_path, kind, child_sha);
                if (kind == 'd') {
                    flatten_tree(repo, child_sha, child_path, out);
                }
            }
        }
        if (!eol) break;
        lp = eol + 1;
    }
    svnae_rep_free(body);
}

static int
flat_cmp(const void *a, const void *b)
{
    const struct flat_entry *pa = a, *pb = b;
    return strcmp(pa->path, pb->path);
}

struct path_change { char action; char *path; };
struct svnae_paths { struct path_change *items; int n; };

struct svnae_paths *
svnae_repos_paths_changed(const char *repo, int rev)
{
    if (rev < 0) return NULL;

    /* Rev 0 is the initial empty-copy revision the seeder writes; its
     * paths-changed list is empty. For rev > 0 we diff against rev-1. */
    char *cur_root = root_dir_sha1_for_rev(repo, rev);
    if (!cur_root) return NULL;
    char *prev_root = NULL;
    if (rev > 0) {
        prev_root = root_dir_sha1_for_rev(repo, rev - 1);
        if (!prev_root) { free(cur_root); return NULL; }
    }

    struct flat_tree cur = {0}, prev = {0};
    flatten_tree(repo, cur_root, "", &cur);
    if (prev_root) flatten_tree(repo, prev_root, "", &prev);
    free(cur_root);
    free(prev_root);

    qsort(cur.items,  (size_t)cur.n,  sizeof *cur.items,  flat_cmp);
    qsort(prev.items, (size_t)prev.n, sizeof *prev.items, flat_cmp);

    struct svnae_paths *P = calloc(1, sizeof *P);
    int cap = cur.n + prev.n + 1;
    P->items = calloc((size_t)cap, sizeof *P->items);

    int i = 0, j = 0;
    while (i < cur.n || j < prev.n) {
        int cmp;
        if      (i >= cur.n)  cmp = +1;
        else if (j >= prev.n) cmp = -1;
        else                  cmp = strcmp(cur.items[i].path, prev.items[j].path);

        if (cmp == 0) {
            if (strcmp(cur.items[i].sha1, prev.items[j].sha1) != 0) {
                P->items[P->n].action = 'M';
                P->items[P->n].path   = strdup(cur.items[i].path);
                P->n++;
            }
            i++; j++;
        } else if (cmp < 0) {
            P->items[P->n].action = 'A';
            P->items[P->n].path   = strdup(cur.items[i].path);
            P->n++;
            i++;
        } else {
            P->items[P->n].action = 'D';
            P->items[P->n].path   = strdup(prev.items[j].path);
            P->n++;
            j++;
        }
    }

    flat_free(&cur);
    flat_free(&prev);
    return P;
}

int svnae_repos_paths_count(const struct svnae_paths *P) { return P ? P->n : 0; }

const char *
svnae_repos_paths_path(const struct svnae_paths *P, int i)
{
    if (!P || i < 0 || i >= P->n) return "";
    return P->items[i].path;
}

/* Returns a single-char action string: "A", "M", or "D". */
const char *
svnae_repos_paths_action(const struct svnae_paths *P, int i)
{
    static const char a_[] = "A", m_[] = "M", d_[] = "D", u_[] = "";
    if (!P || i < 0 || i >= P->n) return u_;
    switch (P->items[i].action) {
        case 'A': return a_;
        case 'M': return m_;
        case 'D': return d_;
        default:  return u_;
    }
}

void
svnae_repos_paths_free(struct svnae_paths *P)
{
    if (!P) return;
    for (int i = 0; i < P->n; i++) free(P->items[i].path);
    free(P->items);
    free(P);
}

/* Public shim for server-side copy: resolve (rev, path) to its sha1 +
 * kind char. Returns 1 on success, 0 on miss. */
int
svnae_repos_resolve(const char *repo, int rev, const char *path,
                    char *out_sha1, char *out_kind)
{
    char *root = root_dir_sha1_for_rev(repo, rev);
    if (!root) return 0;
    char kind = 0;
    char *sha1 = NULL;
    int ok = resolve_path(repo, root, path, &kind, &sha1);
    free(root);
    if (!ok) { free(sha1); return 0; }
    memcpy(out_sha1, sha1, 40);
    out_sha1[40] = '\0';
    *out_kind = kind;
    free(sha1);
    return 1;
}
