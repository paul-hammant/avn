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

/* Read the WC's configured content-address algorithm. Stored in wc.db's
 * info table under key "hash_algo" at checkout time. Defaults to sha1
 * for repos that predate Phase 6.1. Returned pointer is to a static
 * thread-local buffer — copy before another call. */
#include <sqlite3.h>
extern sqlite3 *svnae_wc_db_open(const char *wc_root);
extern void     svnae_wc_db_close(sqlite3 *db);
extern char    *svnae_wc_db_get_info(sqlite3 *db, const char *key);
extern void     svnae_wc_info_free(char *s);

/* Inline digest dispatch so every binary that links this shim gets the
 * algorithm table for free, without relying on ae/subr/checksum/shim.c
 * being pulled into the link. Keeping the list in sync with
 * ae/subr/checksum/shim.c's evp_by_name is the golden list: {sha1,
 * sha256}. */
/* Consolidated in ae/ffi/openssl/shim.c. */
extern char *svnae_openssl_hash_hex(const char *algo, const char *data, int len);
#define svnae_hash_hex svnae_openssl_hash_hex

/* Binary-safe file read via std.fs TLS buffer. */
extern int fs_try_read_binary(const char *path);
extern const char *fs_get_read_binary(void);
extern int fs_get_read_binary_length(void);
extern void fs_release_read_binary(void);

const char *
svnae_wc_hash_algo(const char *wc_root)
{
    static __thread char cache[32];
    cache[0] = '\0';
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) { strcpy(cache, "sha1"); return cache; }
    char *v = svnae_wc_db_get_info(db, "hash_algo");
    svnae_wc_db_close(db);
    if (!v || !*v) {
        strcpy(cache, "sha1");
        svnae_wc_info_free(v);
        return cache;
    }
    size_t n = strlen(v);
    if (n >= sizeof cache) n = sizeof cache - 1;
    memcpy(cache, v, n);
    cache[n] = '\0';
    svnae_wc_info_free(v);
    return cache;
}

/* Compute the WC's configured hash of `data[0..len]`. `out` must be at
 * least 65 bytes. Returns the hex length on success, 0 on failure. */
static int
wc_hash(const char *wc_root, const char *data, int len, char *out)
{
    const char *algo = svnae_wc_hash_algo(wc_root);
    char *hex = svnae_hash_hex(algo, data, len);
    if (!hex) return 0;
    size_t hlen = strlen(hex);
    if (hlen >= 65) { free(hex); return 0; }
    memcpy(out, hex, hlen + 1);
    free(hex);
    return (int)hlen;
}

/* Public: hash `data[0..len]` using the WC's configured algorithm.
 * `out` must be at least 65 bytes. Returns hex length on success, 0
 * on failure. */
int
svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out)
{
    return wc_hash(wc_root, data, len, out);
}

/* Public: hash the contents of `path` on disk using the WC's configured
 * algorithm. `out` must be at least 65 bytes. Returns 0 on success,
 * -1 on I/O or hash failure. */
int
svnae_wc_hash_file(const char *wc_root, const char *path, char *out)
{
    if (!fs_try_read_binary(path)) return -1;
    int sz = fs_get_read_binary_length();
    const char *data = fs_get_read_binary();
    int rc = wc_hash(wc_root, data, sz, out) ? 0 : -1;
    fs_release_read_binary();
    return rc;
}

/* Build $wc_root/.svn/pristine/aa/bb/<sha1>.rep into `out`.
 * Returns the parent directory (through a static buffer, one call at a
 * time). */
/* Path builder ported to Aether (ae/wc/pristine_path.ae, --emit=lib).
 * The C wrapper keeps the caller-supplied-buffer shape so the four
 * call sites don't have to change. */
extern const char *aether_pristine_path(const char *wc_root, const char *sha);
extern const char *aether_pristine_dir(const char *wc_root, const char *sha);

static void
build_path(const char *wc_root, const char *sha1, char *out, size_t out_sz)
{
    const char *p = aether_pristine_path(wc_root, sha1);
    size_t n = strlen(p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

/* mkdir_p + atomic write ported to Aether (ae/subr/io.ae). */
extern int aether_io_mkdir_p(const char *path);
extern int aether_io_write_atomic(const char *path, const char *data, int length);

/* --- public API ------------------------------------------------------- */

/* Put bytes into the pristine store. Returns sha1 as a static
 * thread-local buffer (caller copies if they need to keep it across
 * the next call). NULL on failure. */
const char *
svnae_wc_pristine_put(const char *wc_root, const char *data, int len)
{
    static __thread char sha1[65];   /* sized for sha256 hex */
    if (wc_hash(wc_root, data, len, sha1) == 0) return NULL;

    char path[PATH_MAX];
    build_path(wc_root, sha1, path, sizeof path);

    /* Dedup: if already present, skip the write. */
    extern int aether_io_exists(const char *p);
    if (aether_io_exists(path)) return sha1;

    /* mkdir -p the two-level fanout. */
    if (aether_io_mkdir_p(aether_pristine_dir(wc_root, sha1)) != 0) return NULL;

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

    int rc = aether_io_write_atomic(path, file_buf, file_len);
    free(file_buf);
    if (rc != 0) return NULL;
    return sha1;
}

int
svnae_wc_pristine_has(const char *wc_root, const char *sha1)
{
    extern int aether_io_is_regular_file(const char *path);
    char path[PATH_MAX];
    build_path(wc_root, sha1, path, sizeof path);
    return aether_io_is_regular_file(path);
}

/* Return the uncompressed size recorded in the header, or -1 on failure. */
int
svnae_wc_pristine_size(const char *wc_root, const char *sha1)
{
    char path[PATH_MAX];
    build_path(wc_root, sha1, path, sizeof path);
    if (!fs_try_read_binary(path)) return -1;
    int fsize = fs_get_read_binary_length();
    if (fsize < 5) { fs_release_read_binary(); return -1; }
    const unsigned char *hdr = (const unsigned char *)fs_get_read_binary();
    int len = (int)hdr[1]
            | ((int)hdr[2] << 8)
            | ((int)hdr[3] << 16)
            | ((int)hdr[4] << 24);
    fs_release_read_binary();
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

    /* Binary-safe read via std.fs's TLS-buffered read_binary. */
    if (!fs_try_read_binary(path)) return NULL;
    int fsize = fs_get_read_binary_length();
    if (fsize < 5) { fs_release_read_binary(); return NULL; }
    char *file = malloc((size_t)fsize);
    if (!file) { fs_release_read_binary(); return NULL; }
    memcpy(file, fs_get_read_binary(), (size_t)fsize);
    fs_release_read_binary();

    char header = file[0];
    int uncompressed = (int)(unsigned char)file[1]
                    | ((int)(unsigned char)file[2] << 8)
                    | ((int)(unsigned char)file[3] << 16)
                    | ((int)(unsigned char)file[4] << 24);

    char *out = malloc((size_t)uncompressed + 1);
    if (!out) { free(file); return NULL; }

    if (header == 'R') {
        if (fsize - 5 != uncompressed) { free(file); free(out); return NULL; }
        memcpy(out, file + 5, (size_t)uncompressed);
    } else if (header == 'Z') {
        uLongf produced = (uLongf)uncompressed;
        int rc = uncompress((Bytef *)out, &produced,
                            (const Bytef *)(file + 5),
                            (uLong)(fsize - 5));
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
