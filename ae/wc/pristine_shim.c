/* ae/wc/pristine_shim.c — working-copy pristine store.
 *
 * Mirrors the fs_fs rep-store in layout and byte format, but lives under
 * $wc/.svn/pristine/ and has no rep-cache.db — the file's existence IS
 * the cache. We just compute the SHA-1 of the input, check whether the
 * .rep file exists, and write it if not.
 *
 * On-disk format (same as fs_fs):
 *   $wc/.svn/pristine/aa/bb/<sha1>.rep
 *   First byte: 'R' for raw, 'Z' for zlib. Rest: payload.
 *
 * API:
 *   pristine_put(wc_root, data, len) -> sha1
 *   pristine_get(wc_root, sha1)      -> malloc'd bytes (+ NUL term.)
 *   pristine_free(ptr)
 *   pristine_has(wc_root, sha1)      -> 0/1
 *   pristine_size(wc_root, sha1)     -> decompressed byte count (-1 if missing)
 *
 * We intentionally don't track the decompressed size in a sidecar index:
 * `uncompress` can grow the buffer, so we call it with a size we discover
 * by reading the file's first 4 bytes (deflate block headers) and falling
 * back to an inflating loop. For simplicity at this phase: store the
 * uncompressed length as a 4-byte LE prefix right after the 'R'/'Z' byte.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- helpers ---------------------------------------------------------- */

static void
sha1_hex(const char *data, int len, char out[41])
{
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, data, (size_t)len);
    EVP_DigestFinal_ex(ctx, buf, &n);
    EVP_MD_CTX_free(ctx);
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < 20; i++) {
        out[i*2]   = hex[buf[i] >> 4];
        out[i*2+1] = hex[buf[i] & 0xf];
    }
    out[40] = '\0';
}

/* Build $wc_root/.svn/pristine/aa/bb/<sha1>.rep into `out`.
 * Returns the parent directory (through a static buffer, one call at a
 * time). */
static void
build_path(const char *wc_root, const char *sha1, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s/.svn/pristine/%c%c/%c%c/%s.rep",
             wc_root, sha1[0], sha1[1], sha1[2], sha1[3], sha1);
}

static int
mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
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
write_atomic(const char *path, const char *data, int len)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    const char *p = data;
    int rem = len;
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

/* --- public API ------------------------------------------------------- */

/* Put bytes into the pristine store. Returns sha1 as a static
 * thread-local buffer (caller copies if they need to keep it across
 * the next call). NULL on failure. */
const char *
svnae_wc_pristine_put(const char *wc_root, const char *data, int len)
{
    static __thread char sha1[41];
    sha1_hex(data, len, sha1);

    char path[PATH_MAX];
    build_path(wc_root, sha1, path, sizeof path);

    /* Dedup: if already present, skip the write. */
    struct stat st;
    if (stat(path, &st) == 0) return sha1;

    /* mkdir -p the two-level fanout. */
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/.svn/pristine/%c%c/%c%c",
             wc_root, sha1[0], sha1[1], sha1[2], sha1[3]);
    if (mkdir_p(dir) != 0) return NULL;

    /* Decide RAW vs ZLIB. Same threshold as fs_fs: zlib only if it
     * saves at least 16 bytes. */
    unsigned char *zbuf = NULL;
    uLongf zlen = 0;
    int use_zlib = 0;
    if (len > 0) {
        uLongf bound = compressBound((uLong)len);
        zbuf = malloc(bound);
        if (zbuf) {
            zlen = bound;
            if (compress2(zbuf, &zlen, (const Bytef *)data, (uLong)len, 6) == Z_OK
                && (int)zlen + 16 < len) {
                use_zlib = 1;
            }
        }
    }

    /* File layout: 1 header byte + 4 LE bytes of uncompressed length + payload. */
    int payload_len = use_zlib ? (int)zlen : len;
    int file_len = 1 + 4 + payload_len;
    char *file_buf = malloc((size_t)file_len);
    if (!file_buf) { free(zbuf); return NULL; }
    file_buf[0] = use_zlib ? 'Z' : 'R';
    file_buf[1] = (char)(len & 0xff);
    file_buf[2] = (char)((len >> 8) & 0xff);
    file_buf[3] = (char)((len >> 16) & 0xff);
    file_buf[4] = (char)((len >> 24) & 0xff);
    if (use_zlib) memcpy(file_buf + 5, zbuf, payload_len);
    else          memcpy(file_buf + 5, data, payload_len);
    free(zbuf);

    int rc = write_atomic(path, file_buf, file_len);
    free(file_buf);
    if (rc != 0) return NULL;
    return sha1;
}

int
svnae_wc_pristine_has(const char *wc_root, const char *sha1)
{
    char path[PATH_MAX];
    build_path(wc_root, sha1, path, sizeof path);
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

/* Return the uncompressed size recorded in the header, or -1 on failure. */
int
svnae_wc_pristine_size(const char *wc_root, const char *sha1)
{
    char path[PATH_MAX];
    build_path(wc_root, sha1, path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    unsigned char hdr[5];
    if (read(fd, hdr, sizeof hdr) != (ssize_t)sizeof hdr) { close(fd); return -1; }
    close(fd);
    int len = (int)hdr[1]
            | ((int)hdr[2] << 8)
            | ((int)hdr[3] << 16)
            | ((int)hdr[4] << 24);
    return len;
}

/* Read the pristine blob for `sha1`. Returns malloc'd NUL-terminated
 * buffer (embedded NULs preserved up to the recorded length) or NULL on
 * miss. Caller frees with svnae_wc_pristine_free. */
char *
svnae_wc_pristine_get(const char *wc_root, const char *sha1)
{
    char path[PATH_MAX];
    build_path(wc_root, sha1, path, sizeof path);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    if (st.st_size < 5) { close(fd); return NULL; }

    char *file = malloc((size_t)st.st_size);
    ssize_t got = 0;
    while (got < st.st_size) {
        ssize_t n = read(fd, file + got, (size_t)(st.st_size - got));
        if (n < 0) { if (errno == EINTR) continue; close(fd); free(file); return NULL; }
        if (n == 0) break;
        got += n;
    }
    close(fd);
    if (got != st.st_size) { free(file); return NULL; }

    char header = file[0];
    int uncompressed = (int)(unsigned char)file[1]
                    | ((int)(unsigned char)file[2] << 8)
                    | ((int)(unsigned char)file[3] << 16)
                    | ((int)(unsigned char)file[4] << 24);

    char *out = malloc((size_t)uncompressed + 1);
    if (!out) { free(file); return NULL; }

    if (header == 'R') {
        if ((int)st.st_size - 5 != uncompressed) { free(file); free(out); return NULL; }
        memcpy(out, file + 5, (size_t)uncompressed);
    } else if (header == 'Z') {
        uLongf produced = (uLongf)uncompressed;
        int rc = uncompress((Bytef *)out, &produced,
                            (const Bytef *)(file + 5),
                            (uLong)(st.st_size - 5));
        if (rc != Z_OK || (int)produced != uncompressed) {
            free(file); free(out); return NULL;
        }
    } else {
        free(file); free(out); return NULL;
    }

    out[uncompressed] = '\0';
    free(file);
    return out;
}

void svnae_wc_pristine_free(char *p) { free(p); }
