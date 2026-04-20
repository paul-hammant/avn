/* rep_store_shim.c — C-side rep-store read/write.
 *
 * Why this exists: txn_shim.c's rebuild_dir_c needs to read and write blobs
 * during its recursive work, but the original read/write logic lives on the
 * Aether side. Calling back from C into Aether functions isn't modelled by
 * Aether's FFI. So we maintain a parallel C implementation that matches
 * the on-disk format exactly (the Aether readers can still pick up what
 * this shim writes — they share the format, not the code).
 *
 * Format recap (from ae/fs_fs/test_repo.ae):
 *   $repo/reps/aa/bb/<sha1>.rep: 1-byte header 'R' or 'Z' + payload.
 *     'R' = raw bytes, payload is the uncompressed blob.
 *     'Z' = zlib-compressed payload; uncompressed size comes from the
 *           rep-cache.db row's `uncompressed_size` column.
 *   $repo/rep-cache.db: one row per unique sha1.
 *     (hash TEXT PK, rel_path TEXT, uncompressed_size INT, storage INT).
 *     storage=1=RAW, 2=ZLIB.
 */

#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#define STORAGE_RAW  1
#define STORAGE_ZLIB 2

/* --- utility -------------------------------------------------------- */

static int
write_file_atomic(const char *path, const char *data, int len)
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
    if (close(fd) != 0) return -errno;
    if (rename(tmp, path) != 0) { unlink(tmp); return -errno; }
    return 0;
}

static int
write_file_with_header_atomic(const char *path, char header, const char *data, int len)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    (void)write(fd, &header, 1);
    const char *p = data;
    int rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -errno; }
        p += w; rem -= (int)w;
    }
    if (fsync(fd) != 0) { int rc = -errno; close(fd); unlink(tmp); return rc; }
    if (close(fd) != 0) return -errno;
    if (rename(tmp, path) != 0) { unlink(tmp); return -errno; }
    return 0;
}

/* Build $repo/reps/aa/bb/... and mkdir -p those directories. */
static int
mkdir_p(const char *path)
{
    /* Quick: assume parents mostly exist; try each segment. */
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -errno;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -errno;
    return 0;
}

/* Parse the second token out of $repo/format. The file looks like:
 *   svnae-fsfs-1\n                 (legacy — assumed sha1)
 *   svnae-fsfs-1 sha256\n          (primary = sha256)
 *   svnae-fsfs-1 sha256,sha1\n     (primary = sha256, sha1 kept as secondary)
 * Returns the primary algorithm name as a static-thread-local string
 * (caller copies before another call). On any parse failure or if the
 * file is missing, returns "sha1" for backward compatibility. */
const char *
svnae_repo_primary_hash(const char *repo)
{
    static __thread char cache[32];
    cache[0] = '\0';

    char fmt_path[PATH_MAX];
    snprintf(fmt_path, sizeof fmt_path, "%s/format", repo);
    FILE *f = fopen(fmt_path, "r");
    if (!f) { strcpy(cache, "sha1"); return cache; }
    char line[128];
    if (!fgets(line, sizeof line, f)) { fclose(f); strcpy(cache, "sha1"); return cache; }
    fclose(f);

    /* Trim trailing whitespace. */
    size_t n = strlen(line);
    while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' ')) line[--n] = '\0';

    /* Look for a space after the tag. */
    char *sp = strchr(line, ' ');
    if (!sp) { strcpy(cache, "sha1"); return cache; }
    const char *algos = sp + 1;
    /* Primary is up to the first ',' (or end of line). */
    const char *comma = strchr(algos, ',');
    size_t plen = comma ? (size_t)(comma - algos) : strlen(algos);
    if (plen == 0 || plen >= sizeof cache) { strcpy(cache, "sha1"); return cache; }
    memcpy(cache, algos, plen);
    cache[plen] = '\0';
    return cache;
}

/* Hash `data` using the repo's primary algorithm. `out` must be at
 * least 65 bytes (64 hex chars for sha256 + NUL). Returns the hex
 * length on success, 0 on failure. Inlines the golden list here —
 * matches ae/subr/checksum/shim.c. */
static int
repo_hash_of(const char *repo, const char *data, int len, char out[65])
{
    const char *algo = svnae_repo_primary_hash(repo);
    const EVP_MD *md = NULL;
    if      (strcmp(algo, "sha1")   == 0) md = EVP_sha1();
    else if (strcmp(algo, "sha256") == 0) md = EVP_sha256();
    if (!md) return 0;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return 0;
    unsigned char dig[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    int ok = (EVP_DigestInit_ex(ctx, md, NULL) == 1
              && EVP_DigestUpdate(ctx, data, (size_t)len) == 1
              && EVP_DigestFinal_ex(ctx, dig, &dlen) == 1);
    EVP_MD_CTX_free(ctx);
    if (!ok) return 0;
    if ((int)dlen * 2 >= 65) return 0;
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < dlen; i++) {
        out[i * 2]     = hex[dig[i] >> 4];
        out[i * 2 + 1] = hex[dig[i] & 0x0f];
    }
    out[dlen * 2] = '\0';
    return (int)dlen * 2;
}

/* --- rep-cache access ---------------------------------------------- */

static int
rep_cache_lookup(sqlite3 *db, const char *sha1_hex, int *storage, int *uncompressed)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT storage, uncompressed_size FROM rep_cache WHERE hash = ?",
            -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, sha1_hex, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    int found = 0;
    if (rc == SQLITE_ROW) {
        *storage = sqlite3_column_int(st, 0);
        *uncompressed = sqlite3_column_int(st, 1);
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

static int
rep_cache_insert(sqlite3 *db, const char *sha1_hex, const char *rel_path,
                 int uncompressed_size, int storage)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO rep_cache (hash, rel_path, uncompressed_size, storage) VALUES (?, ?, ?, ?)",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, sha1_hex,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, rel_path,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 3, uncompressed_size);
    sqlite3_bind_int (st, 4, storage);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* --- public interface ---------------------------------------------- */

/* Write `data[0..len]` to the rep store (dedup by SHA-1). Returns a pointer
 * to a static-thread-local buffer containing the hex SHA-1. Caller must
 * copy before making another call. Returns NULL on failure. */
const char *
svnae_rep_write_blob(const char *repo, const char *data, int len)
{
    static __thread char sha1_buf[65];   /* sized for sha256 (64 hex chars + NUL) */
    if (repo_hash_of(repo, data, len, sha1_buf) == 0) return NULL;

    char cache_path[PATH_MAX];
    snprintf(cache_path, sizeof cache_path, "%s/rep-cache.db", repo);

    sqlite3 *db;
    if (sqlite3_open_v2(cache_path, &db,
            SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    int storage, uncompressed;
    if (rep_cache_lookup(db, sha1_buf, &storage, &uncompressed)) {
        sqlite3_close(db);
        return sha1_buf;
    }

    /* Decide RAW vs ZLIB. */
    int use_zlib = 0;
    unsigned char *zbuf = NULL;
    uLongf zlen = 0;
    if (len > 0) {
        uLongf bound = compressBound((uLong)len);
        zbuf = malloc(bound);
        if (zbuf) {
            zlen = bound;
            if (compress2(zbuf, &zlen, (const Bytef *)data, (uLong)len, 6) == Z_OK) {
                if ((int)zlen + 16 < len) use_zlib = 1;
            }
        }
    }

    /* Write .rep file. */
    char rel_path[80];
    snprintf(rel_path, sizeof rel_path,
             "%c%c/%c%c/%s.rep",
             sha1_buf[0], sha1_buf[1], sha1_buf[2], sha1_buf[3], sha1_buf);

    char full_path[PATH_MAX];
    snprintf(full_path, sizeof full_path, "%s/reps/%s", repo, rel_path);

    /* mkdir -p the parent dirs. */
    char parent[PATH_MAX];
    snprintf(parent, sizeof parent, "%s/reps/%c%c/%c%c",
             repo, sha1_buf[0], sha1_buf[1], sha1_buf[2], sha1_buf[3]);
    if (mkdir_p(parent) != 0) {
        free(zbuf); sqlite3_close(db); return NULL;
    }

    int wrc;
    if (use_zlib) {
        wrc = write_file_with_header_atomic(full_path, 'Z', (const char *)zbuf, (int)zlen);
    } else {
        wrc = write_file_with_header_atomic(full_path, 'R', data, len);
    }
    free(zbuf);
    if (wrc != 0) { sqlite3_close(db); return NULL; }

    if (rep_cache_insert(db, sha1_buf, rel_path, len,
                         use_zlib ? STORAGE_ZLIB : STORAGE_RAW) != 0) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_close(db);
    return sha1_buf;
}

/* Read a blob from the rep store. Returns a malloc'd NUL-terminated buffer
 * (embedded NULs allowed; caller uses strlen only if they know the content
 * is text). Returns NULL on miss. The caller must free() the result. */
char *
svnae_rep_read_blob(const char *repo, const char *sha1_hex)
{
    char cache_path[PATH_MAX];
    snprintf(cache_path, sizeof cache_path, "%s/rep-cache.db", repo);

    sqlite3 *db;
    if (sqlite3_open_v2(cache_path, &db,
            SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    int storage = 0, uncompressed = 0;
    if (!rep_cache_lookup(db, sha1_hex, &storage, &uncompressed)) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_close(db);

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/reps/%c%c/%c%c/%s.rep",
             repo, sha1_hex[0], sha1_hex[1], sha1_hex[2], sha1_hex[3], sha1_hex);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    size_t fsize = (size_t)st.st_size;
    if (fsize < 1) { close(fd); return NULL; }

    char *file_buf = malloc(fsize);
    if (!file_buf) { close(fd); return NULL; }
    size_t got = 0;
    while (got < fsize) {
        ssize_t n = read(fd, file_buf + got, fsize - got);
        if (n < 0) { if (errno == EINTR) continue; free(file_buf); close(fd); return NULL; }
        if (n == 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != fsize) { free(file_buf); return NULL; }

    char header = file_buf[0];
    const char *payload = file_buf + 1;
    int payload_len = (int)fsize - 1;

    if (header == 'R') {
        char *out = malloc((size_t)payload_len + 1);
        if (!out) { free(file_buf); return NULL; }
        memcpy(out, payload, (size_t)payload_len);
        out[payload_len] = '\0';
        free(file_buf);
        return out;
    }
    if (header == 'Z') {
        char *out = malloc((size_t)uncompressed + 1);
        if (!out) { free(file_buf); return NULL; }
        uLongf produced = (uLongf)uncompressed;
        if (uncompress((Bytef *)out, &produced,
                       (const Bytef *)payload, (uLong)payload_len) != Z_OK
            || (int)produced != uncompressed) {
            free(out); free(file_buf); return NULL;
        }
        out[uncompressed] = '\0';
        free(file_buf);
        return out;
    }
    free(file_buf);
    return NULL;
}

/* A small helper the test uses to free the malloc'd output of
 * svnae_rep_read_blob / svnae_txn_rebuild_root. */
void svnae_rep_free(char *p) { free(p); }

/* Count .rep files under $repo/reps. Used by the test to prove
 * rep-sharing: the delta between two counts is the number of new
 * unique blobs written by a commit. */
#include <dirent.h>

static int
count_reps_recurse(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int c = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            c += count_reps_recurse(p);
        } else {
            size_t nl = strlen(e->d_name);
            if (nl > 4 && strcmp(e->d_name + nl - 4, ".rep") == 0) c++;
        }
    }
    closedir(d);
    return c;
}

int
svnae_count_rep_files(const char *repo)
{
    char reps[PATH_MAX];
    snprintf(reps, sizeof reps, "%s/reps", repo);
    return count_reps_recurse(reps);
}
