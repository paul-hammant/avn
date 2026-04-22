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

/* Atomic write + mkdir-p ported to Aether (ae/subr/io.ae). */
extern int aether_io_write_atomic(const char *path, const char *data, int length);
extern int aether_io_mkdir_p(const char *path);

static int write_file_atomic(const char *path, const char *data, int len)
{
    return aether_io_write_atomic(path, data, len) == 0 ? 0 : -1;
}

/* rep blob layout: 1 header byte + payload. The Aether atomic-write
 * is binary-safe, so we just prepend the header to a single buffer
 * and write it in one go. Cleaner than the original two-syscall
 * version (header write then payload loop). */
static int
write_file_with_header_atomic(const char *path, char header, const char *data, int len)
{
    char *buf = malloc((size_t)len + 1);
    if (!buf) return -1;
    buf[0] = header;
    if (len > 0) memcpy(buf + 1, data, (size_t)len);
    int rc = aether_io_write_atomic(path, buf, len + 1);
    free(buf);
    return rc == 0 ? 0 : -1;
}

static int mkdir_p(const char *path) { return aether_io_mkdir_p(path) == 0 ? 0 : -1; }

/* Format-line parsing ported to Aether (ae/fs_fs/format_line.ae). */
extern const char *aether_format_primary_hash(const char *line);
extern int         aether_format_secondary_count(const char *line);
extern const char *aether_format_secondary_hash(const char *line, int i);

/* Read + trim the first line of $repo/format. Returns 0 on success
 * with the trimmed line in `out` (out_sz bytes), -1 if absent. */
static int
read_format_line(const char *repo, char *out, size_t out_sz)
{
    char fmt_path[PATH_MAX];
    snprintf(fmt_path, sizeof fmt_path, "%s/format", repo);
    FILE *f = fopen(fmt_path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)out_sz, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' || out[n-1] == ' ')) out[--n] = '\0';
    return 0;
}

const char *
svnae_repo_primary_hash(const char *repo)
{
    static __thread char cache[32];
    char line[128];
    if (read_format_line(repo, line, sizeof line) != 0) {
        strcpy(cache, "sha1"); return cache;
    }
    const char *primary = aether_format_primary_hash(line);
    if (!primary || !*primary) { strcpy(cache, "sha1"); return cache; }
    size_t plen = strlen(primary);
    if (plen >= sizeof cache) { strcpy(cache, "sha1"); return cache; }
    memcpy(cache, primary, plen + 1);
    return cache;
}

int
svnae_repo_secondary_hashes(const char *repo, char out[4][32])
{
    char line[256];
    if (read_format_line(repo, line, sizeof line) != 0) return 0;
    int n = aether_format_secondary_count(line);
    if (n > 4) n = 4;
    int count = 0;
    for (int i = 0; i < n; i++) {
        const char *s = aether_format_secondary_hash(line, i);
        size_t sl = strlen(s);
        if (sl == 0 || sl >= 32) break;
        memcpy(out[count], s, sl + 1);
        count++;
    }
    return count;
}

/* Hex-encode digest into out (>= 65 bytes). */
/* Hash → hex lives in ae/ffi/openssl/shim.c; thin alias kept for
 * back-compat with the call sites in this file. */
extern int svnae_openssl_hash_hex_into(const char *algo, const char *data, int len, char *out);

static int
hex_of_algo(const char *algo, const char *data, int len, char out[65])
{
    return svnae_openssl_hash_hex_into(algo, data, len, out);
}

/* Hash `data` using the repo's primary algorithm. `out` must be at
 * least 65 bytes (64 hex chars for sha256 + NUL). Returns the hex
 * length on success, 0 on failure. Inlines the golden list here —
 * matches ae/subr/checksum/shim.c. */
static int
repo_hash_of(const char *repo, const char *data, int len, char out[65])
{
    return hex_of_algo(svnae_repo_primary_hash(repo), data, len, out);
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

/* Ensure the secondary-hash table exists. Created lazily on the first
 * secondary-hash write so repos that never grew secondaries don't
 * carry the schema. Not an error if it already exists. */
static void
rep_cache_sec_ensure(sqlite3 *db)
{
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS rep_cache_sec ("
        "  primary_hash TEXT NOT NULL,"
        "  algo         TEXT NOT NULL,"
        "  secondary_hash TEXT NOT NULL,"
        "  PRIMARY KEY (primary_hash, algo))",
        NULL, NULL, NULL);
}

static int
rep_cache_sec_insert(sqlite3 *db, const char *primary_hex,
                     const char *algo, const char *secondary_hex)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO rep_cache_sec (primary_hash, algo, secondary_hash) VALUES (?,?,?)",
            -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, primary_hex,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, algo,          -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, secondary_hex, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Lookup: returns malloc'd hex or NULL. Caller frees. */
char *
svnae_rep_lookup_secondary(const char *repo, const char *primary_hex,
                          const char *algo)
{
    char cache_path[PATH_MAX];
    snprintf(cache_path, sizeof cache_path, "%s/rep-cache.db", repo);
    sqlite3 *db;
    if (sqlite3_open_v2(cache_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT secondary_hash FROM rep_cache_sec WHERE primary_hash = ? AND algo = ?",
            -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_bind_text(st, 1, primary_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, algo,        -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        if (s) out = strdup(s);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return out;
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

    /* Path assembly ported to Aether (ae/fs_fs/reppath.ae). */
    extern const char *aether_rep_rel_path(const char *sha);
    extern const char *aether_rep_path(const char *repo, const char *sha);
    extern const char *aether_rep_dir(const char *repo, const char *sha);
    char rel_path[80];
    {
        const char *rp = aether_rep_rel_path(sha1_buf);
        size_t n = strlen(rp);
        if (n >= sizeof rel_path) { free(zbuf); sqlite3_close(db); return NULL; }
        memcpy(rel_path, rp, n + 1);
    }
    char full_path[PATH_MAX];
    {
        const char *fp = aether_rep_path(repo, sha1_buf);
        size_t n = strlen(fp);
        if (n >= sizeof full_path) { free(zbuf); sqlite3_close(db); return NULL; }
        memcpy(full_path, fp, n + 1);
    }

    /* mkdir -p the parent dirs. */
    if (mkdir_p(aether_rep_dir(repo, sha1_buf)) != 0) {
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

    /* Phase 7.5: compute and persist secondary hashes (if any). The
     * table is created on demand so legacy repos pay no cost. */
    char sec[4][32];
    int sec_n = svnae_repo_secondary_hashes(repo, sec);
    if (sec_n > 0) {
        rep_cache_sec_ensure(db);
        for (int i = 0; i < sec_n; i++) {
            char shex[65];
            if (hex_of_algo(sec[i], data, len, shex)) {
                rep_cache_sec_insert(db, sha1_buf, sec[i], shex);
            }
        }
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

    extern const char *aether_rep_path(const char *repo, const char *sha);
    char path[PATH_MAX];
    {
        const char *rp = aether_rep_path(repo, sha1_hex);
        size_t n = strlen(rp);
        if (n >= sizeof path) return NULL;
        memcpy(path, rp, n + 1);
    }

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
