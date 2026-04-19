/* compress/shim.c — one-shot deflate / inflate of a byte buffer via zlib.
 *
 * svn uses zlib in svndiff windows (libsvn_delta) and in fs_fs representation
 * storage. Phase 0.4 ships the whole-buffer shape; streaming is a Phase 2
 * concern.
 *
 * The tricky part for the Aether bridge is that compressed bytes can contain
 * embedded NULs, so we can't pass them back as NUL-terminated strings.
 * Solution: the shim returns an opaque handle (`struct svnae_buf`) that
 * carries {bytes, length}. Aether sees it as `ptr` and calls accessor
 * functions to extract what it needs. The Aether side never tries to treat
 * the raw byte range as a NUL-terminated string — it can still ask for the
 * bytes as a `string` for round-tripping through test code (where we know
 * they're ASCII) or it can pass the handle back to inflate for the round
 * trip. No length-encoded prefix hack.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

struct svnae_buf {
    char *data;      /* malloc'd, exactly `length` bytes */
    int   length;
};

static struct svnae_buf *
buf_new(const unsigned char *src, int n)
{
    struct svnae_buf *b = malloc(sizeof *b);
    if (!b) return NULL;
    /* Always allocate n+1 and NUL-terminate so buf_data returns a C string
     * that's safe to print even when the payload has no trailing NUL.
     * Embedded NULs still propagate via buf_length. */
    b->data = malloc((size_t)n + 1);
    if (!b->data) { free(b); return NULL; }
    if (n > 0) memcpy(b->data, src, n);
    b->data[n] = '\0';
    b->length = n;
    return b;
}

/* Deflate `data[0..data_len]` at zlib level `level` (0..9). */
struct svnae_buf *
svnae_zlib_deflate(const char *data, int data_len, int level)
{
    uLongf bound = compressBound((uLong)data_len);
    unsigned char *tmp = malloc(bound);
    if (!tmp) return NULL;
    uLongf produced = bound;
    int rc = compress2(tmp, &produced, (const Bytef *)data, (uLong)data_len, level);
    if (rc != Z_OK) { free(tmp); return NULL; }
    struct svnae_buf *b = buf_new(tmp, (int)produced);
    free(tmp);
    return b;
}

/* Inflate a buffer previously produced by svnae_zlib_deflate, given the
 * expected output size (caller tracks the original length — svn does this
 * at every call site where it cares). */
struct svnae_buf *
svnae_zlib_inflate(const char *data, int data_len, int expected_out)
{
    unsigned char *out = malloc(expected_out > 0 ? expected_out : 1);
    if (!out) return NULL;
    uLongf produced = (uLongf)expected_out;
    int rc = uncompress(out, &produced, (const Bytef *)data, (uLong)data_len);
    if (rc != Z_OK || (int)produced != expected_out) { free(out); return NULL; }
    struct svnae_buf *b = buf_new(out, (int)produced);
    free(out);
    return b;
}

/* Accessors exposed to Aether as `extern`s. */
int         svnae_buf_length(const struct svnae_buf *b) { return b ? b->length : 0; }
const char *svnae_buf_data  (const struct svnae_buf *b) { return b ? b->data : ""; }

/* Same as svnae_zlib_inflate but with an explicit offset into the input
 * buffer — lets callers point past a header byte without building a new
 * buffer on the Aether side. */
struct svnae_buf *
svnae_zlib_inflate_offset(const char *base, int offset, int data_len, int expected_out)
{
    return svnae_zlib_inflate(base + offset, data_len, expected_out);
}

/* Write `header` (NUL-terminated) followed by the contents of `buf` (which
 * may contain embedded NULs) to `path`. Used by fs_fs to persist a
 * 1-byte header + zlib payload without routing the binary payload through
 * Aether's strlen-based string pipeline. */
int
svnae_write_header_and_buf(const char *path, const char *header, const struct svnae_buf *buf)
{
    if (!buf) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;

    int header_len = (int)strlen(header);
    const char *p = header;
    int rem = header_len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -errno; }
        p += w; rem -= (int)w;
    }

    p = buf->data;
    rem = buf->length;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); return -errno; }
        p += w; rem -= (int)w;
    }

    if (fsync(fd) != 0) { int rc = -errno; close(fd); return rc; }
    if (close(fd) != 0) return -errno;
    return 0;
}

void
svnae_buf_free(struct svnae_buf *b)
{
    if (!b) return;
    free(b->data);
    free(b);
}
