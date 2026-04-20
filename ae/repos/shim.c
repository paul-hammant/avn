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
