/* fs_fs/txn_shim.c — transaction accumulator.
 *
 * A txn holds a base revision and a list of pending edits. Edits are:
 *     ADD_FILE    path, content-bytes          — create a new file
 *     MOD_FILE    path, content-bytes          — replace an existing file
 *                                                 (same representation on-wire)
 *     MKDIR       path                         — create an empty directory
 *     DELETE      path                         — remove a file or directory
 *
 * Aether can't cheaply allocate byte blobs that travel through function
 * calls, so content for ADD_FILE/MOD_FILE is taken via `const char *` and
 * an explicit length (binary-safe).
 *
 * At commit time the Aether side walks the edit list bottom-up, applying
 * them against the base rev's tree. That walk is written in Aether; this
 * shim just provides storage for the edit list and a couple of read
 * accessors (so Aether can iterate without juggling arrays).
 *
 * We reuse the `struct svnae_buf` shape from the rest of the port so the
 * Aether `ptr` opaque handle means the same thing everywhere.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct svnae_buf {
    char *data;
    int   length;
};

static struct svnae_buf *buf_from(const unsigned char *src, int n) {
    struct svnae_buf *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->data = malloc((size_t)n + 1);
    if (!b->data) { free(b); return NULL; }
    if (n > 0) memcpy(b->data, src, n);
    b->data[n] = '\0';
    b->length = n;
    return b;
}

/* --- edit list --------------------------------------------------------- */

enum edit_kind {
    EDIT_ADD_FILE = 1,   /* ADD or replace; fs_fs storage is the same */
    EDIT_MKDIR    = 2,
    EDIT_DELETE   = 3,
};

struct edit {
    int   kind;
    char *path;      /* canonical: no leading '/', '/' separator, no trailing '/' */
    char *content;   /* only for ADD_FILE; malloc'd, binary-safe */
    int   content_len;
};

struct svnae_txn {
    int   base_rev;
    struct edit *edits;
    int   n;
    int   cap;
};

struct svnae_txn *
svnae_txn_new(int base_rev)
{
    struct svnae_txn *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->base_rev = base_rev;
    return t;
}

int svnae_txn_base_rev(const struct svnae_txn *t) { return t ? t->base_rev : -1; }

static int
push_edit(struct svnae_txn *t)
{
    if (t->n == t->cap) {
        int ncap = t->cap ? t->cap * 2 : 8;
        struct edit *p = realloc(t->edits, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        t->edits = p;
        t->cap = ncap;
    }
    return t->n++;
}

int
svnae_txn_add_file(struct svnae_txn *t, const char *path, const char *content, int len)
{
    if (!t) return -1;
    int idx = push_edit(t);
    if (idx < 0) return -1;
    struct edit *e = &t->edits[idx];
    e->kind = EDIT_ADD_FILE;
    e->path = strdup(path);
    if (len > 0 && content) {
        e->content = malloc((size_t)len);
        if (!e->content) { free(e->path); return -1; }
        memcpy(e->content, content, (size_t)len);
        e->content_len = len;
    } else {
        e->content = NULL;
        e->content_len = 0;
    }
    return 0;
}

int
svnae_txn_mkdir(struct svnae_txn *t, const char *path)
{
    if (!t) return -1;
    int idx = push_edit(t);
    if (idx < 0) return -1;
    struct edit *e = &t->edits[idx];
    e->kind = EDIT_MKDIR;
    e->path = strdup(path);
    e->content = NULL;
    e->content_len = 0;
    return 0;
}

int
svnae_txn_delete(struct svnae_txn *t, const char *path)
{
    if (!t) return -1;
    int idx = push_edit(t);
    if (idx < 0) return -1;
    struct edit *e = &t->edits[idx];
    e->kind = EDIT_DELETE;
    e->path = strdup(path);
    e->content = NULL;
    e->content_len = 0;
    return 0;
}

int svnae_txn_count(const struct svnae_txn *t) { return t ? t->n : 0; }

int
svnae_txn_edit_kind(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return -1;
    return t->edits[i].kind;
}

const char *
svnae_txn_edit_path(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return "";
    return t->edits[i].path;
}

struct svnae_buf *
svnae_txn_edit_content(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return NULL;
    return buf_from((const unsigned char *)t->edits[i].content,
                    t->edits[i].content_len);
}

void
svnae_txn_free(struct svnae_txn *t)
{
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        free(t->edits[i].path);
        free(t->edits[i].content);
    }
    free(t->edits);
    free(t);
}

/* ---- a small dynamic string list ------------------------------------- *
 *
 * The commit algorithm gathers all paths affecting a given directory
 * (either directly or via a subpath) and groups them. A plain sorted-
 * unique list of strings is cheaper than our line-oriented sbuilders for
 * set operations.
 */

struct svnae_strset {
    char **items;
    int    n;
    int    cap;
};

struct svnae_strset *svnae_strset_new(void) { return calloc(1, sizeof(struct svnae_strset)); }

int
svnae_strset_add(struct svnae_strset *s, const char *v)
{
    if (!s) return -1;
    /* Skip duplicates. */
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->items[i], v) == 0) return 0;
    if (s->n == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 8;
        char **p = realloc(s->items, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        s->items = p;
        s->cap = ncap;
    }
    s->items[s->n++] = strdup(v);
    return 0;
}

int svnae_strset_count(const struct svnae_strset *s) { return s ? s->n : 0; }

const char *
svnae_strset_get(const struct svnae_strset *s, int i)
{
    if (!s || i < 0 || i >= s->n) return "";
    return s->items[i];
}

void
svnae_strset_free(struct svnae_strset *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) free(s->items[i]);
    free(s->items);
    free(s);
}

/* ---- rebuild_dir in C --------------------------------------------------
 *
 * The commit algorithm is pointer-heavy string processing. Aether's forward-
 * reference return-type inference can't handle a recursive function that
 * returns string (see AETHER_ISSUES.md #1 / forward-decl limitations), so we
 * do the recursion here in C and expose a single entry point.
 *
 * The rebuild_dir C function needs callbacks into Aether for:
 *   read_blob(sha1) -> body
 *   write_blob_text(data) -> sha1
 *   write_blob_bytes(data, len) -> sha1
 * plus access to the txn's edit list.
 *
 * Rather than invent a callback mechanism (which Aether externs don't model),
 * we pass in already-hashed SHA-1s for the file contents: Aether does the
 * hashing/writing of the LEAF blobs (files) up front, populating the txn's
 * content field with a sentinel that encodes the already-written sha1.
 *
 * Actually simpler: the shim calls back out to a pair of C helpers that
 * shell-out to a small set of exported C functions exposed by test code /
 * Aether shims. Cleanest form: have Aether install the repo path before
 * calling rebuild, then the shim reads/writes blobs directly via file I/O
 * and a small rep-cache protocol.
 *
 * For Phase 3.3 we do the simplest thing: the Aether side pre-hashes every
 * file's content (writing to the rep store), and sets the txn entry's
 * content to the sha1 as plain ASCII. The shim then:
 *   - for reads: uses a helper that reads+decompresses a rep blob given
 *     only the repo path and the sha1.
 *   - for writes (directory blobs): produces text + writes via the same
 *     helpers.
 *
 * That requires replicating the rep-store read/write in C. We do that in
 * a minimal way — no rep-cache.db involvement for reads (we compute the
 * path from sha1 + check on-disk), and for writes we call back into a
 * small helper exposed below.
 */

/* Forward-declared helpers we expect to exist (from elsewhere in the
 * program's link closure): */
char       *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
const char *svnae_rep_write_blob(const char *repo, const char *data, int len);

/* Split a txn's edit list into lookup helpers. */
static int
txn_find_edit(const struct svnae_txn *t, const char *path, int *out_kind)
{
    for (int i = 0; i < t->n; i++) {
        if (strcmp(t->edits[i].path, path) == 0) {
            *out_kind = t->edits[i].kind;
            return i;
        }
    }
    *out_kind = 0;
    return -1;
}

/* Does any edit path match `prefix` or start with `prefix + "/"`? */
static int
edits_touch_subtree_c(const struct svnae_txn *t, const char *prefix)
{
    size_t plen = strlen(prefix);
    for (int i = 0; i < t->n; i++) {
        const char *p = t->edits[i].path;
        if (plen == 0) return 1;
        if (strncmp(p, prefix, plen) == 0) {
            if (p[plen] == '\0' || p[plen] == '/') return 1;
        }
    }
    return 0;
}

/* Is `path` an immediate child of `dir_prefix`?
 * Immediate child of "" = path has no '/'.
 * Immediate child of "a/b" = path == "a/b/X" with X containing no '/'. */
static int
is_immediate_child_c(const char *path, const char *dir_prefix)
{
    size_t plen = strlen(path);
    size_t dlen = strlen(dir_prefix);
    if (dlen == 0) {
        return strchr(path, '/') == NULL ? 1 : 0;
    }
    if (plen <= dlen + 1) return 0;
    if (strncmp(path, dir_prefix, dlen) != 0) return 0;
    if (path[dlen] != '/') return 0;
    if (strchr(path + dlen + 1, '/') != NULL) return 0;
    return 1;
}

/* Extract the basename of `path` assuming `dir_prefix` is its parent. */
static const char *
basename_after_c(const char *path, const char *dir_prefix)
{
    size_t dlen = strlen(dir_prefix);
    if (dlen == 0) return path;
    return path + dlen + 1;
}

/* A growable char buffer. */
struct sbuf { char *data; int len, cap; };
static void sbuf_push(struct sbuf *s, const char *t, int n)
{
    if (s->len + n + 1 > s->cap) {
        int nc = s->cap ? s->cap * 2 : 128;
        while (nc < s->len + n + 1) nc *= 2;
        s->data = realloc(s->data, nc);
        s->cap = nc;
    }
    memcpy(s->data + s->len, t, n);
    s->len += n;
    s->data[s->len] = '\0';
}
static void sbuf_push_cstr(struct sbuf *s, const char *t) { sbuf_push(s, t, (int)strlen(t)); }

/* A simple sorted list of (name, kind, sha1) records. */
struct rec { char *name; char kind; char sha1[41]; };
struct reclist { struct rec *items; int n, cap; };

static void reclist_add(struct reclist *r, const char *name, char kind, const char *sha1)
{
    /* Replace if name already present. */
    for (int i = 0; i < r->n; i++) {
        if (strcmp(r->items[i].name, name) == 0) {
            r->items[i].kind = kind;
            memcpy(r->items[i].sha1, sha1, 40);
            r->items[i].sha1[40] = '\0';
            return;
        }
    }
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 8;
        r->items = realloc(r->items, (size_t)nc * sizeof *r->items);
        r->cap = nc;
    }
    r->items[r->n].name = strdup(name);
    r->items[r->n].kind = kind;
    memcpy(r->items[r->n].sha1, sha1, 40);
    r->items[r->n].sha1[40] = '\0';
    r->n++;
}

static void reclist_remove(struct reclist *r, const char *name)
{
    for (int i = 0; i < r->n; i++) {
        if (strcmp(r->items[i].name, name) == 0) {
            free(r->items[i].name);
            r->items[i] = r->items[r->n - 1];
            r->n--;
            return;
        }
    }
}

static int rec_cmp(const void *a, const void *b)
{
    return strcmp(((const struct rec *)a)->name, ((const struct rec *)b)->name);
}

static void reclist_free(struct reclist *r)
{
    for (int i = 0; i < r->n; i++) free(r->items[i].name);
    free(r->items);
}

/* Recursively rebuild a directory.
 * Returns a malloc'd 40-char SHA-1 string (caller frees), or NULL on error.
 * `base_dir_sha1_or_null` may be "" meaning "no base dir here". */
static char *
rebuild_dir_c(const char *repo, const char *base_dir_sha1,
              const char *prefix, const struct svnae_txn *txn)
{
    struct reclist rl = {0};

    /* Step 1: start from base entries. */
    if (base_dir_sha1 && base_dir_sha1[0]) {
        char *base_body = svnae_rep_read_blob(repo, base_dir_sha1);
        if (base_body) {
            /* Parse each line "<k> <sha1> <name>\n". */
            char *p = base_body;
            while (*p) {
                char *eol = strchr(p, '\n');
                if (!eol) break;
                int llen = (int)(eol - p);
                if (llen < 44) { p = eol + 1; continue; }
                char kind = p[0];
                char sha1[41];
                memcpy(sha1, p + 2, 40);
                sha1[40] = '\0';
                int name_len = llen - 43;
                char *name = malloc((size_t)name_len + 1);
                memcpy(name, p + 43, (size_t)name_len);
                name[name_len] = '\0';

                /* full_path = prefix ? prefix+"/"+name : name */
                const char *full_path;
                char *full_alloc = NULL;
                if (prefix[0]) {
                    size_t pl = strlen(prefix);
                    full_alloc = malloc(pl + 1 + (size_t)name_len + 1);
                    memcpy(full_alloc, prefix, pl);
                    full_alloc[pl] = '/';
                    memcpy(full_alloc + pl + 1, name, (size_t)name_len);
                    full_alloc[pl + 1 + name_len] = '\0';
                    full_path = full_alloc;
                } else {
                    full_path = name;
                }

                int k;
                int idx = txn_find_edit(txn, full_path, &k);
                if (idx >= 0 && k == EDIT_DELETE) {
                    /* skip */
                } else if (idx >= 0 && k == EDIT_ADD_FILE) {
                    /* override — handled in step 2 */
                } else if (kind == 'd' && edits_touch_subtree_c(txn, full_path)) {
                    char *new_sub = rebuild_dir_c(repo, sha1, full_path, txn);
                    if (new_sub) {
                        reclist_add(&rl, name, 'd', new_sub);
                        free(new_sub);
                    } else {
                        /* Empty subtree — keep as empty dir. */
                        const char *empty_sha = svnae_rep_write_blob(repo, "", 0);
                        reclist_add(&rl, name, 'd', empty_sha);
                    }
                } else {
                    reclist_add(&rl, name, kind, sha1);
                }

                free(name);
                free(full_alloc);
                p = eol + 1;
            }
            free(base_body);
        }
    }

    /* Step 2: apply edits whose immediate parent is this dir. */
    for (int i = 0; i < txn->n; i++) {
        const struct edit *e = &txn->edits[i];
        if (!is_immediate_child_c(e->path, prefix)) continue;
        const char *name = basename_after_c(e->path, prefix);
        if (e->kind == EDIT_ADD_FILE) {
            const char *sha1 = svnae_rep_write_blob(repo, e->content, e->content_len);
            if (!sha1) continue;
            reclist_add(&rl, name, 'f', sha1);
        } else if (e->kind == EDIT_MKDIR) {
            /* Avoid overwriting an existing dir entry whose SHA will be
             * computed by recursion on descendants. */
            int exists = 0;
            for (int j = 0; j < rl.n; j++)
                if (strcmp(rl.items[j].name, name) == 0 && rl.items[j].kind == 'd') { exists = 1; break; }
            if (!exists) {
                const char *empty = svnae_rep_write_blob(repo, "", 0);
                reclist_add(&rl, name, 'd', empty);
            }
        } else if (e->kind == EDIT_DELETE) {
            reclist_remove(&rl, name);
        }
    }

    /* Step 3: also handle edits that create entries in sub-trees that
     * don't yet exist in base (e.g. add_file inside a just-mkdir'd dir).
     * We need to recurse into those new subtrees too. */
    for (int i = 0; i < rl.n; i++) {
        if (rl.items[i].kind != 'd') continue;
        /* Compute the full path this entry represents. */
        char *subprefix;
        if (prefix[0]) {
            size_t pl = strlen(prefix), nl = strlen(rl.items[i].name);
            subprefix = malloc(pl + 1 + nl + 1);
            memcpy(subprefix, prefix, pl);
            subprefix[pl] = '/';
            memcpy(subprefix + pl + 1, rl.items[i].name, nl + 1);
        } else {
            subprefix = strdup(rl.items[i].name);
        }

        /* If any edit is rooted inside this subprefix AND the dir's current
         * sha1 is the empty-dir one OR hasn't been recursed-through above,
         * run the recursion now using the current sha1 as the base. */
        if (edits_touch_subtree_c(txn, subprefix)) {
            char *new_sub = rebuild_dir_c(repo, rl.items[i].sha1, subprefix, txn);
            if (new_sub) {
                memcpy(rl.items[i].sha1, new_sub, 40);
                rl.items[i].sha1[40] = '\0';
                free(new_sub);
            }
        }
        free(subprefix);
    }

    /* Step 4: sort and serialise. */
    qsort(rl.items, (size_t)rl.n, sizeof *rl.items, rec_cmp);

    struct sbuf body = {0};
    for (int i = 0; i < rl.n; i++) {
        char line[80];
        snprintf(line, sizeof line, "%c %.40s %s\n",
                 rl.items[i].kind, rl.items[i].sha1, rl.items[i].name);
        sbuf_push_cstr(&body, line);
    }

    const char *dir_sha = svnae_rep_write_blob(repo, body.data ? body.data : "", body.len);
    char *out = dir_sha ? strdup(dir_sha) : NULL;

    free(body.data);
    reclist_free(&rl);
    return out;
}

/* Aether-facing entry point. Returns a malloc'd 40-char hex SHA-1 (Aether
 * takes ownership and will eventually need to free it via a helper; for
 * tests we accept the tiny leak). Returns NULL on failure. */
char *
svnae_txn_rebuild_root(const char *repo, const char *base_root_sha1,
                       const struct svnae_txn *txn)
{
    return rebuild_dir_c(repo, base_root_sha1, "", txn);
}

