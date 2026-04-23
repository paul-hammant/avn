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

/* Dir-blob line parser ported to Aether (ae/fs_fs/dirblob.ae). */
extern int         aether_dir_count_entries(const char *body);
extern int         aether_dir_entry_kind(const char *body, int i);
extern const char *aether_dir_entry_name(const char *body, int i);
void  svnae_rep_free(char *p);

/* --- helpers --------------------------------------------------------- */

/* Slurp an entire small file ported to Aether (ae/subr/io.ae). The
 * head_rev + rev_blob_sha1 ported to ae/repos/rev_io.ae
 * (repos_head_rev / repos_rev_blob_sha). Wrappers preserve the
 * existing int / char* conventions; read_small / parse_int /
 * trim_trailing_newline / rev_pointer_path have no remaining
 * callers and were removed. */
extern int         aether_repos_head_rev(const char *repo);
extern const char *aether_repos_rev_blob_sha(const char *repo, int rev);

static int
head_rev(const char *repo) { return aether_repos_head_rev(repo); }

static char *
rev_blob_sha1(const char *repo, int rev) {
    const char *sha = aether_repos_rev_blob_sha(repo, rev);
    if (!sha || !*sha) return NULL;
    return strdup(sha);
}

/* "key: value" extractor — ported to Aether (ae/repos/blobfield.ae).
 * Wrapper preserves the original "malloc or NULL" contract: Aether
 * returns "" for both missing and empty; no caller in the port
 * differentiates, so collapse to one case. */
extern const char *aether_blobfield_get(const char *body, const char *key);

static char *
parse_field(const char *body, const char *key)
{
    if (!body) return NULL;
    const char *v = aether_blobfield_get(body, key);
    if (!v || !*v) return NULL;
    return strdup(v);
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

extern const char *aether_repos_load_rev_blob_field(const char *repo, int rev,
                                                    const char *key);

static char *
root_dir_sha1_for_rev(const char *repo, int rev)
{
    const char *v = aether_repos_load_rev_blob_field(repo, rev, "root");
    if (!v || !*v) return NULL;
    return strdup(v);
}

/* resolve_path ported to Aether (ae/repos/resolve.ae). The split-
 * accessor shape (resolve_kind first, resolve_sha second) replaces
 * the pair of out-params. Both calls walk the path again, but at
 * these path lengths (typically 2-4 segments) that's negligible. */
extern int         aether_resolve_kind(const char *repo, const char *root_sha, const char *path);
extern const char *aether_resolve_sha (const char *repo, const char *root_sha, const char *path);

static int
resolve_path(const char *repo, const char *root_sha1, const char *path,
             char *out_kind, char **out_sha1)
{
    *out_kind = 0;
    *out_sha1 = NULL;
    int k = aether_resolve_kind(repo, root_sha1, path);
    if (k == 0) return 0;
    *out_kind = (char)k;
    const char *sha = aether_resolve_sha(repo, root_sha1, path);
    *out_sha1 = strdup(sha ? sha : "");
    return 1;
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
    int n_entries = aether_dir_count_entries(body);
    L->items = calloc((size_t)(n_entries > 0 ? n_entries : 1), sizeof *L->items);
    for (int ei = 0; ei < n_entries; ei++) {
        L->items[ei].kind = (char)aether_dir_entry_kind(body, ei);
        L->items[ei].name = strdup(aether_dir_entry_name(body, ei));
    }
    L->n = n_entries;
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
    /* If the rev pointer doesn't exist at all, bail. load_rev_blob_field
     * returns "" for "rev exists but field absent" and also "" for
     * "rev doesn't exist" — distinguish the two by probing for the
     * "root" field, which every valid rev blob carries. */
    const char *root = aether_repos_load_rev_blob_field(repo, rev, "root");
    if (!root || !*root) return NULL;

    struct svnae_info *I = calloc(1, sizeof *I);
    I->rev = rev;

    const char *v;
    v = aether_repos_load_rev_blob_field(repo, rev, "author");
    I->author = (v && *v) ? strdup(v) : NULL;
    v = aether_repos_load_rev_blob_field(repo, rev, "date");
    I->date = (v && *v) ? strdup(v) : NULL;
    v = aether_repos_load_rev_blob_field(repo, rev, "log");
    I->msg = (v && *v) ? strdup(v) : NULL;
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

/* flat_tree + flatten_tree + flat_add + flat_free + flat_cmp all
 * ported to Aether (ae/repos/paths_changed.ae). */

struct path_change { char action; char *path; };
struct svnae_paths { struct path_change *items; int n; };

/* Paths-changed diff ported to Aether (ae/repos/paths_changed.ae).
 * Aether side flattens both trees, sorts, and merge-walks. Returns
 * "ACTION\nPATH\n..." — we parse it into the C svnae_paths struct. */
extern const char *aether_paths_changed_between(const char *repo,
                                                const char *prev_root_sha,
                                                const char *cur_root_sha);

struct svnae_paths *
svnae_repos_paths_changed(const char *repo, int rev)
{
    if (rev < 0) return NULL;

    char *cur_root = root_dir_sha1_for_rev(repo, rev);
    if (!cur_root) return NULL;
    char *prev_root = NULL;
    if (rev > 0) {
        prev_root = root_dir_sha1_for_rev(repo, rev - 1);
        if (!prev_root) { free(cur_root); return NULL; }
    }

    const char *diff = aether_paths_changed_between(repo,
                                                    prev_root ? prev_root : "",
                                                    cur_root);
    free(cur_root);
    free(prev_root);

    /* Parse "ACTION\nPATH\n" line pairs into the svnae_paths list. */
    struct svnae_paths *P = calloc(1, sizeof *P);
    if (!P) return NULL;
    int cap = 0;
    const char *p = diff ? diff : "";
    while (*p) {
        char action = *p;
        if (*(p+1) != '\n') break;
        p += 2;
        const char *path_start = p;
        while (*p && *p != '\n') p++;
        int path_len = (int)(p - path_start);
        if (*p == '\n') p++;

        if (P->n == cap) {
            cap = cap ? cap * 2 : 16;
            P->items = realloc(P->items, (size_t)cap * sizeof *P->items);
        }
        P->items[P->n].action = action;
        P->items[P->n].path = malloc((size_t)path_len + 1);
        memcpy(P->items[P->n].path, path_start, (size_t)path_len);
        P->items[P->n].path[path_len] = '\0';
        P->n++;
    }
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
    size_t sl = strlen(sha1);
    /* out_sha1 must be big enough — callers now pass char[65]. */
    memcpy(out_sha1, sha1, sl + 1);
    *out_kind = kind;
    free(sha1);
    return 1;
}

/* Aether-callable split-accessor form. Returns 0 on miss instead of
 * an out-param pair. kind is 'f'(102), 'd'(100), or 0. */
int
svnae_repos_resolve_kind(const char *repo, int rev, const char *path)
{
    char sha[65]; char kind = 0;
    if (!svnae_repos_resolve(repo, rev, path, sha, &kind)) return 0;
    return (int)(unsigned char)kind;
}

const char *
svnae_repos_resolve_sha(const char *repo, int rev, const char *path)
{
    static __thread char buf[65];
    buf[0] = '\0';
    char kind = 0;
    svnae_repos_resolve(repo, rev, path, buf, &kind);
    return buf;
}

/* --- blame -----------------------------------------------------------
 *
 * Per-line attribution: for every line of `path` at `rev`, determine
 * the rev that first introduced that exact line.
 *
 * Algorithm, classic and straightforward:
 *   1. Walk revs 1..rev. For each rev where `path` changed (per
 *      svnae_repos_paths_changed), fetch both the prior and current
 *      content.
 *   2. Run a line-level LCS diff. Lines that survive from prior
 *      keep their annotation; newly-added lines get (rev, author).
 *   3. When the walk reaches `rev`, emit the final attribution list.
 *
 * The line annotations live in a simple array parallel to the lines
 * of the file's current content at each rev. When the file is
 * modified, we recompute. This is O(revs * lines^2) in the worst
 * case; fine for small-to-medium files. Myers diff would bring it
 * down to O(revs * (lines + edits)) but adds significant complexity.
 */

struct line_ann {
    int   rev;
    char *author;   /* borrowed from log; copied into the blame result */
    char *text;     /* malloc'd copy of the line (no trailing \n) */
};

struct svnae_blame {
    struct line_ann *items;
    int n, cap;
};

static void
ann_push(struct svnae_blame *B, int rev, const char *author, const char *text, int tlen)
{
    if (B->n == B->cap) {
        int nc = B->cap ? B->cap * 2 : 64;
        B->items = realloc(B->items, (size_t)nc * sizeof *B->items);
        B->cap = nc;
    }
    B->items[B->n].rev = rev;
    B->items[B->n].author = author ? strdup(author) : strdup("");
    B->items[B->n].text = malloc((size_t)tlen + 1);
    memcpy(B->items[B->n].text, text, (size_t)tlen);
    B->items[B->n].text[tlen] = '\0';
    B->n++;
}

/* Split `data[0..len]` into line ranges. Returns a malloc'd array of
 * offsets into data + a parallel array of lengths. Trailing '\n' is
 * NOT included in the line range. `*out_n` is the line count. */
static void
split_lines(const char *data, int len, int **out_off, int **out_len, int *out_n)
{
    int cap = 32, n = 0;
    int *off = malloc((size_t)cap * sizeof *off);
    int *lns = malloc((size_t)cap * sizeof *lns);
    int i = 0;
    while (i < len) {
        int start = i;
        while (i < len && data[i] != '\n') i++;
        if (n == cap) {
            cap *= 2;
            off = realloc(off, (size_t)cap * sizeof *off);
            lns = realloc(lns, (size_t)cap * sizeof *lns);
        }
        off[n] = start;
        lns[n] = i - start;
        n++;
        if (i < len) i++;  /* skip '\n' */
    }
    *out_off = off;
    *out_len = lns;
    *out_n = n;
}

/* Line-level LCS to find common subsequence between `a` and `b`.
 * Returns a malloc'd int array `match[0..na-1]` where match[i] is
 * the index in b of a[i]'s match, or -1 if a[i] is unique to a.
 * Standard dynamic-programming LCS; O(na*nb) time and space. */
static int *
lcs_match(const char *a_data, int *a_off, int *a_len, int na,
          const char *b_data, int *b_off, int *b_len, int nb)
{
    /* dp[i][j] = length of LCS of a[0..i-1] and b[0..j-1]. */
    int *dp = calloc((size_t)(na + 1) * (size_t)(nb + 1), sizeof *dp);
    if (!dp) return NULL;
    #define DP(i, j) dp[(i) * (nb + 1) + (j)]
    for (int i = 1; i <= na; i++) {
        for (int j = 1; j <= nb; j++) {
            if (a_len[i-1] == b_len[j-1]
                && memcmp(a_data + a_off[i-1], b_data + b_off[j-1], (size_t)a_len[i-1]) == 0) {
                DP(i, j) = DP(i-1, j-1) + 1;
            } else {
                int u = DP(i-1, j);
                int v = DP(i, j-1);
                DP(i, j) = u > v ? u : v;
            }
        }
    }
    int *match = malloc((size_t)na * sizeof *match);
    for (int i = 0; i < na; i++) match[i] = -1;
    int i = na, j = nb;
    while (i > 0 && j > 0) {
        if (a_len[i-1] == b_len[j-1]
            && memcmp(a_data + a_off[i-1], b_data + b_off[j-1], (size_t)a_len[i-1]) == 0) {
            match[i-1] = j - 1;
            i--; j--;
        } else if (DP(i-1, j) >= DP(i, j-1)) {
            i--;
        } else {
            j--;
        }
    }
    #undef DP
    free(dp);
    return match;
}

struct svnae_blame *
svnae_repos_blame(const char *repo, int target_rev, const char *path)
{
    if (target_rev < 0) target_rev = head_rev(repo);
    if (target_rev < 1) return NULL;

    /* Walk revs 1..target_rev, maintaining a current annotation list
     * that mirrors the file's content at each step. Skip revs that
     * don't touch `path`. */
    struct svnae_blame *B = calloc(1, sizeof *B);
    char *prev_body = NULL;
    int   prev_len  = 0;
    /* Parallel to prev_body's lines: each line's attribution. */
    struct line_ann *prev_ann = NULL;
    int prev_ann_n = 0;

    for (int r = 1; r <= target_rev; r++) {
        /* Did this rev touch `path`? */
        struct svnae_paths *P = svnae_repos_paths_changed(repo, r);
        int touches = 0;
        char action = 0;
        if (P) {
            int pn = svnae_repos_paths_count(P);
            for (int i = 0; i < pn; i++) {
                if (strcmp(svnae_repos_paths_path(P, i), path) == 0) {
                    touches = 1;
                    action = svnae_repos_paths_action(P, i)[0];
                    break;
                }
            }
            svnae_repos_paths_free(P);
        }
        if (!touches) continue;
        if (action == 'D') {
            /* Deleted in this rev — clear state. If later added back,
             * we start fresh. */
            free(prev_body); prev_body = NULL; prev_len = 0;
            for (int i = 0; i < prev_ann_n; i++) {
                free(prev_ann[i].author); free(prev_ann[i].text);
            }
            free(prev_ann); prev_ann = NULL; prev_ann_n = 0;
            continue;
        }

        /* Fetch content + author for this rev. */
        char *body = svnae_repos_cat(repo, r, path);
        if (!body) continue;
        int body_len = (int)strlen(body);

        struct svnae_info *I = svnae_repos_info_rev(repo, r);
        const char *author = I ? svnae_repos_info_author(I) : "";

        int *cur_off = NULL, *cur_lens = NULL, cur_n = 0;
        split_lines(body, body_len, &cur_off, &cur_lens, &cur_n);

        struct line_ann *cur_ann = calloc((size_t)(cur_n > 0 ? cur_n : 1), sizeof *cur_ann);

        if (prev_body == NULL) {
            /* First version — every line gets this rev. */
            for (int i = 0; i < cur_n; i++) {
                cur_ann[i].rev = r;
                cur_ann[i].author = strdup(author);
                cur_ann[i].text = malloc((size_t)cur_lens[i] + 1);
                memcpy(cur_ann[i].text, body + cur_off[i], (size_t)cur_lens[i]);
                cur_ann[i].text[cur_lens[i]] = '\0';
            }
        } else {
            /* Diff prev_body → body, carry forward matched lines. */
            int *prev_off = NULL, *prev_lns = NULL, prev_n = 0;
            split_lines(prev_body, prev_len, &prev_off, &prev_lns, &prev_n);
            /* LCS maps each cur line back to a prev line (or -1). */
            int *match = lcs_match(body, cur_off, cur_lens, cur_n,
                                   prev_body, prev_off, prev_lns, prev_n);
            for (int i = 0; i < cur_n; i++) {
                cur_ann[i].text = malloc((size_t)cur_lens[i] + 1);
                memcpy(cur_ann[i].text, body + cur_off[i], (size_t)cur_lens[i]);
                cur_ann[i].text[cur_lens[i]] = '\0';
                if (match && match[i] >= 0 && match[i] < prev_ann_n) {
                    cur_ann[i].rev = prev_ann[match[i]].rev;
                    cur_ann[i].author = strdup(prev_ann[match[i]].author);
                } else {
                    cur_ann[i].rev = r;
                    cur_ann[i].author = strdup(author);
                }
            }
            free(match);
            free(prev_off); free(prev_lns);
        }

        if (I) svnae_repos_info_free(I);

        /* Free prev, swap cur → prev. */
        free(prev_body);
        for (int i = 0; i < prev_ann_n; i++) {
            free(prev_ann[i].author); free(prev_ann[i].text);
        }
        free(prev_ann);
        prev_body = body;
        prev_len = body_len;
        prev_ann = cur_ann;
        prev_ann_n = cur_n;
        free(cur_off); free(cur_lens);
    }

    /* Final: copy prev_ann into B. */
    for (int i = 0; i < prev_ann_n; i++) {
        ann_push(B, prev_ann[i].rev, prev_ann[i].author,
                 prev_ann[i].text, (int)strlen(prev_ann[i].text));
        free(prev_ann[i].author); free(prev_ann[i].text);
    }
    free(prev_ann);
    free(prev_body);
    return B;
}

int         svnae_blame_count(const struct svnae_blame *B) { return B ? B->n : 0; }
int         svnae_blame_rev  (const struct svnae_blame *B, int i) {
    return (B && i >= 0 && i < B->n) ? B->items[i].rev : -1;
}
const char *svnae_blame_author(const struct svnae_blame *B, int i) {
    return (B && i >= 0 && i < B->n) ? B->items[i].author : "";
}
const char *svnae_blame_text (const struct svnae_blame *B, int i) {
    return (B && i >= 0 && i < B->n) ? B->items[i].text : "";
}
void svnae_blame_free(struct svnae_blame *B) {
    if (!B) return;
    for (int i = 0; i < B->n; i++) {
        free(B->items[i].author);
        free(B->items[i].text);
    }
    free(B->items);
    free(B);
}
