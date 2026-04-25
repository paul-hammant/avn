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
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define STORAGE_RAW  1
#define STORAGE_ZLIB 2

/* --- Aether bridge ---------------------------------------------------- */

/* Blob encode/write/read ported to ae/fs_fs/rep_store.ae using
 * std.zlib + std.fs. Encoded envelope from rep_encode_blob is
 * "<use_zlib>\x01<bytes>" where use_zlib is '0' or '1' — the
 * helpers below read the flag + slice the body, then we pass the
 * body to rep_write_encoded (which atomically writes via
 * fs.write_binary). */
extern const char *aether_rep_encode_blob(const char *data, int length);
extern int         aether_rep_encoded_use_zlib(const char *encoded);
extern const char *aether_rep_write_encoded(const char *repo, const char *sha,
                                             const char *encoded);
extern const char *aether_rep_read_decoded(const char *repo, const char *sha,
                                            int uncompressed_size);

/* aether_string_data / aether_string_length come from std/string/
 * aether_string.h (added upstream in Aether 0.91 to replace the
 * open-coded magic-header unwrap pattern that lived in this file
 * since round 29). They accept either AetherString* or raw char*,
 * are NULL-safe, and are length-aware (no strlen pitfall on payloads
 * with embedded NULs). */
#include "aether_string.h"

/* Binary-safe concat / slice helpers — called from the Aether-
 * generated pristine_generated.c and rep_store_generated.c. Both
 * shim files are linked into every binary that uses either
 * generated.c, so the `aether_pristine_*` names resolve uniquely
 * from rep_store_shim.c. std.string has no byte-construction
 * primitive that doesn't route through a NUL-terminated C-style
 * string; these do.
 *
 * Named aether_pristine_* for historical reasons (originated in
 * pristine_shim.c during round 29); worth a rename to
 * aether_bin_* if we ever touch every call site.
 *
 * string_new_with_length is declared in aether_string.h (already
 * included above). */

const char *
aether_pristine_concat_binary(const char *prefix, const char *suf, int suf_len)
{
    const char *pdata = aether_string_data(prefix);
    int         plen  = (int)aether_string_length(prefix);

    const char *sdata = aether_string_data(suf);
    int         slen  = (int)aether_string_length(suf);
    if (slen < suf_len) suf_len = slen;

    int total = plen + suf_len;
    char *buf = malloc((size_t)total + 1);
    if (!buf) return (const char *)string_new_with_length("", 0);
    if (plen) memcpy(buf,        pdata, (size_t)plen);
    if (suf_len) memcpy(buf + plen, sdata, (size_t)suf_len);
    buf[total] = '\0';
    const char *out = (const char *)string_new_with_length(buf, total);
    free(buf);
    return out;
}

const char *
aether_pristine_slice_binary(const char *s, int start, int end)
{
    const char *data = aether_string_data(s);
    int         len  = (int)aether_string_length(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (end < start) end = start;
    return (const char *)string_new_with_length(data + start, end - start);
}


/* Format-line parsing initially ported to ae/fs_fs/format_line.ae,
 * then superseded by ae/repos/rev_io.ae's repo_primary_hash /
 * repo_secondary_hashes_joined which do the full file read + parse
 * in one call. No C caller of the line-level parsers remains.
 *
 * Ported to ae/repos/rev_io.ae::repo_primary_hash. Still returns a
 * TLS-cached string so call sites that hold the pointer across a
 * subsequent call don't trample it. */
extern const char *aether_repo_primary_hash(const char *repo);

const char *
svnae_repo_primary_hash(const char *repo)
{
    static __thread char cache[32];
    const char *v = aether_repo_primary_hash(repo);
    const char *src = (v && *v) ? v : "sha1";
    size_t n = strlen(src);
    if (n >= sizeof cache) n = sizeof cache - 1;
    memcpy(cache, src, n);
    cache[n] = '\0';
    return cache;
}

/* Parse ported to ae/repos/rev_io.ae::repo_secondary_hashes_joined
 * (returns \n-separated names); splitter stays here because Aether
 * can't hand C a fixed char[4][32] array directly. */
extern const char *aether_repo_secondary_hashes_joined(const char *repo);

int
svnae_repo_secondary_hashes(const char *repo, char out[4][32])
{
    const char *joined = aether_repo_secondary_hashes_joined(repo);
    if (!joined || !*joined) return 0;
    int count = 0;
    const char *p = joined;
    while (*p && count < 4) {
        const char *eol = strchr(p, '\n');
        size_t slen = eol ? (size_t)(eol - p) : strlen(p);
        if (slen == 0 || slen >= 32) break;
        memcpy(out[count], p, slen);
        out[count][slen] = '\0';
        count++;
        if (!eol) break;
        p = eol + 1;
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

/* --- rep-cache access ---------------------------------------------- *
 *
 * Primary rep_cache lookup/insert moved to ae/fs_fs/rep_store.ae
 * (rep_cache_has / rep_cache_lookup_uncompressed / rep_cache_insert).
 * The secondary-hash table remains here for now: svnae_rep_lookup_secondary
 * has a caller in svnserver/shim.c and the write path is driven from
 * this file's already-open sqlite handle when secondary hashes are
 * configured. */

extern int aether_rep_cache_has(const char *repo, const char *sha);
extern int aether_rep_cache_lookup_uncompressed(const char *repo, const char *sha);
extern int aether_rep_cache_insert(const char *repo, const char *sha,
                                    const char *rel_path,
                                    int uncompressed, int storage);

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

    if (aether_rep_cache_has(repo, sha1_buf)) return sha1_buf;

    /* Hand encode + atomic binary write to ae/fs_fs/rep_store.ae.
     * Aether's envelope carries use_zlib back so we update
     * rep-cache.db's `storage` column without re-inspecting the
     * encoded payload. */
    extern const char *aether_rep_rel_path(const char *sha);
    const char *encoded = aether_rep_encode_blob(data, len);
    if (!encoded) return NULL;
    int use_zlib = aether_rep_encoded_use_zlib(encoded);

    const char *werr = aether_rep_write_encoded(repo, sha1_buf, encoded);
    if (werr && aether_string_length(werr) > 0) return NULL;

    /* rel_path for the rep-cache.db row. */
    char rel_path[80];
    {
        const char *rp = aether_rep_rel_path(sha1_buf);
        size_t n = strlen(rp);
        if (n >= sizeof rel_path) return NULL;
        memcpy(rel_path, rp, n + 1);
    }

    if (aether_rep_cache_insert(repo, sha1_buf, rel_path, len,
                                 use_zlib ? STORAGE_ZLIB : STORAGE_RAW) != 0) {
        return NULL;
    }

    /* Phase 7.5: compute and persist secondary hashes (if any). The
     * table is created on demand so legacy repos pay no cost. */
    char sec[4][32];
    int sec_n = svnae_repo_secondary_hashes(repo, sec);
    if (sec_n > 0) {
        char cache_path[PATH_MAX];
        snprintf(cache_path, sizeof cache_path, "%s/rep-cache.db", repo);
        sqlite3 *db = NULL;
        if (sqlite3_open_v2(cache_path, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
            rep_cache_sec_ensure(db);
            for (int i = 0; i < sec_n; i++) {
                char shex[65];
                if (hex_of_algo(sec[i], data, len, shex)) {
                    rep_cache_sec_insert(db, sha1_buf, sec[i], shex);
                }
            }
            sqlite3_close(db);
        }
    }
    return sha1_buf;
}

/* Read a blob from the rep store. Returns a malloc'd NUL-terminated buffer
 * (embedded NULs allowed; caller uses strlen only if they know the content
 * is text). Returns NULL on miss. The caller must free() the result. */
char *
svnae_rep_read_blob(const char *repo, const char *sha1_hex)
{
    int uncompressed = aether_rep_cache_lookup_uncompressed(repo, sha1_hex);
    if (uncompressed < 0) return NULL;

    /* Hand the file read + header dispatch + zlib inflate to Aether.
     * uncompressed_size comes from rep-cache.db. "" on miss /
     * corruption; length may contain embedded NULs otherwise. */
    const char *decoded = aether_rep_read_decoded(repo, sha1_hex, uncompressed);
    const char *ddata = aether_string_data(decoded);
    int         dlen  = (int)aether_string_length(decoded);
    if (dlen == 0 && uncompressed == 0) {
        /* Empty blob is legitimate. */
        char *out = malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    if (dlen == 0) return NULL;
    char *out = malloc((size_t)dlen + 1);
    if (!out) return NULL;
    memcpy(out, ddata, (size_t)dlen);
    out[dlen] = '\0';
    return out;
}

/* A small helper the test uses to free the malloc'd output of
 * svnae_rep_read_blob / svnae_txn_rebuild_root. */
void svnae_rep_free(char *p) { free(p); }

/* Count .rep files under $repo/reps. Used by the test to prove
 * rep-sharing: the delta between two counts is the number of new
 * unique blobs written by a commit. */
/* Ported to ae/fs_fs/count_reps.ae. (Export symbol is
 * fs_count_rep_files to avoid collision with the test_txn.ae
 * helper of the same name in svnae_count-aware test binaries.) */
extern int aether_fs_count_rep_files(const char *repo);

int
svnae_count_rep_files(const char *repo)
{
    return aether_fs_count_rep_files(repo);
}
