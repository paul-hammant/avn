/* fs_fs/shim.c — helpers for Phase 3.2 that don't belong in subr shims:
 * rev-file ops, sorted directory serialization, and a small list builder
 * so Aether can assemble a tree without juggling C arrays.
 *
 * Everything else (read/write blobs, rep-sharing, zlib) is already in
 * ae/fs_fs/test_repo.ae's inlined logic + ae/subr shims. We keep the
 * shim narrow: write a short file with fsync + rename, read the same
 * back, atomically bump $repo/head, and provide the tree-builder
 * scaffolding.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Same svnae_buf layout as everywhere else. */
struct svnae_buf {
    char *data;
    int   length;
};

static struct svnae_buf *
buf_from(const unsigned char *src, int n)
{
    struct svnae_buf *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->data = malloc((size_t)n + 1);
    if (!b->data) { free(b); return NULL; }
    if (n > 0) memcpy(b->data, src, n);
    b->data[n] = '\0';
    b->length = n;
    return b;
}

/* Read the full file at `path` into a buf. Binary-safe but in Phase 3.2
 * we only read plain-text revision/head files, so Aether strings work. */
struct svnae_buf *
svnae_fsfs_read_small_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    size_t size = (size_t)st.st_size;
    char *mem = malloc(size + 1);
    if (!mem) { close(fd); return NULL; }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, mem + got, size - got);
        if (n < 0) { if (errno == EINTR) continue; free(mem); close(fd); return NULL; }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != size) { free(mem); return NULL; }
    mem[size] = '\0';
    struct svnae_buf *b = buf_from((const unsigned char *)mem, (int)size);
    free(mem);
    return b;
}

int         svnae_fsfs_buf_length(const struct svnae_buf *b) { return b ? b->length : 0; }
const char *svnae_fsfs_buf_data  (const struct svnae_buf *b) { return b ? b->data : ""; }
void        svnae_fsfs_buf_free  (struct svnae_buf *b)
{
    if (!b) return;
    free(b->data);
    free(b);
}

/* Current ISO-8601 UTC timestamp, without milliseconds. For the revision
 * blob's `date:` field. Returned string is static and must be used before
 * the next call. */
const char *
svnae_fsfs_now_iso8601(void)
{
    static char buf[32];
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/* ---- tree builder ----------------------------------------------------- *
 *
 * Aether callers accumulate (path, kind, content) triples into a builder.
 * Kind: 0 = file (content is raw bytes), 1 = directory (content ignored).
 * After adding all entries, the Aether side walks the builder to group
 * children by parent and serialise directory blobs bottom-up.
 *
 * Strings are strdup'd so the caller can free its buffers immediately.
 * Directories need not be listed explicitly — they're inferred from the
 * path components of any files added. If you want an empty directory
 * you do need to add it with kind=1, though.
 *
 * We keep this tiny and naive for Phase 3.2: O(n^2) scans, no hashing,
 * because repos under test have tens of entries not millions. A real
 * commit path will use svn's skel/index infrastructure later.
 */

struct tree_entry {
    char *path;      /* "a/b/c.txt" — no leading slash, '/' separator */
    int   kind;      /* 0 = file, 1 = dir */
    char *content;   /* malloc'd, length = content_len. NULL for dirs. */
    int   content_len;
};

struct svnae_tree_builder {
    struct tree_entry *entries;
    int                n;
    int                cap;
};

struct svnae_tree_builder *
svnae_fsfs_tree_builder_new(void)
{
    return calloc(1, sizeof(struct svnae_tree_builder));
}

int
svnae_fsfs_tree_builder_add(struct svnae_tree_builder *tb,
                            const char *path, int kind,
                            const char *content, int content_len)
{
    if (!tb) return -1;
    if (tb->n == tb->cap) {
        int ncap = tb->cap ? tb->cap * 2 : 8;
        struct tree_entry *p = realloc(tb->entries, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        tb->entries = p;
        tb->cap = ncap;
    }
    struct tree_entry *e = &tb->entries[tb->n];
    e->path = strdup(path);
    e->kind = kind;
    if (kind == 0 && content_len > 0 && content) {
        e->content = malloc((size_t)content_len);
        if (!e->content) { free(e->path); return -1; }
        memcpy(e->content, content, (size_t)content_len);
        e->content_len = content_len;
    } else {
        e->content = NULL;
        e->content_len = 0;
    }
    tb->n++;
    return 0;
}

int
svnae_fsfs_tree_builder_count(const struct svnae_tree_builder *tb)
{
    return tb ? tb->n : 0;
}

const char *
svnae_fsfs_tree_builder_path(const struct svnae_tree_builder *tb, int i)
{
    if (!tb || i < 0 || i >= tb->n) return "";
    return tb->entries[i].path;
}

int
svnae_fsfs_tree_builder_kind(const struct svnae_tree_builder *tb, int i)
{
    if (!tb || i < 0 || i >= tb->n) return -1;
    return tb->entries[i].kind;
}

/* Return the file content as a buf so Aether can pass it into write_blob
 * via the usual handle. Caller frees with svnae_fsfs_buf_free. */
struct svnae_buf *
svnae_fsfs_tree_builder_content(const struct svnae_tree_builder *tb, int i)
{
    if (!tb || i < 0 || i >= tb->n) return NULL;
    return buf_from((const unsigned char *)tb->entries[i].content,
                    tb->entries[i].content_len);
}

void
svnae_fsfs_tree_builder_free(struct svnae_tree_builder *tb)
{
    if (!tb) return;
    for (int i = 0; i < tb->n; i++) {
        free(tb->entries[i].path);
        free(tb->entries[i].content);
    }
    free(tb->entries);
    free(tb);
}

/* ---- string-list helper (for line-by-line building) ------------------- *
 *
 * When building a directory-blob body line-by-line, we need to hand a
 * grown-up string back to Aether. Rather than repeatedly concatenating
 * in Aether (each concat allocates), provide a growable C buffer we push
 * to one line at a time. */

struct svnae_sbuilder {
    char *data;
    int   len, cap;
};

struct svnae_sbuilder *svnae_fsfs_sb_new(void) {
    return calloc(1, sizeof(struct svnae_sbuilder));
}

static int
sb_reserve(struct svnae_sbuilder *sb, int extra)
{
    int need = sb->len + extra + 1;
    if (need <= sb->cap) return 0;
    int ncap = sb->cap ? sb->cap : 64;
    while (ncap < need) ncap *= 2;
    char *p = realloc(sb->data, (size_t)ncap);
    if (!p) return -1;
    sb->data = p;
    sb->cap = ncap;
    return 0;
}

int
svnae_fsfs_sb_push(struct svnae_sbuilder *sb, const char *s)
{
    if (!sb) return -1;
    int n = (int)strlen(s);
    if (sb_reserve(sb, n) != 0) return -1;
    memcpy(sb->data + sb->len, s, (size_t)n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

const char *svnae_fsfs_sb_data  (const struct svnae_sbuilder *sb) { return sb && sb->data ? sb->data : ""; }
int         svnae_fsfs_sb_length(const struct svnae_sbuilder *sb) { return sb ? sb->len : 0; }

void svnae_fsfs_sb_free(struct svnae_sbuilder *sb) {
    if (!sb) return;
    free(sb->data);
    free(sb);
}
