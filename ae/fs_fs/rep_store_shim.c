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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- Aether bridge ---------------------------------------------------- */

/* Blob read lives in ae/fs_fs/rep_store.ae (std.zlib + std.fs). The
 * encode + write side moved entirely to Aether in Round 105 — see
 * rep_write_blob there. The decode side stays C-glued because the
 * read returns a length-aware AetherString that we copy into a
 * malloc'd, NUL-terminated buffer at the FFI boundary. */
extern const char *aether_rep_read_decoded(const char *repo, const char *sha,
                                            int uncompressed_size);

#include "aether_string.h"

/* Binary-safe concat + slice for pristine_generated.c and
 * rep_store_generated.c. std.string has no byte-construction
 * primitive that handles embedded NULs; these route through
 * string_new_with_length (length-aware). Named aether_pristine_*
 * for historical reasons; pristine_shim.c was the original site. */

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


/* Backed by ae/repos/rev_io.ae::repo_primary_hash. TLS-cached so
 * callers can hold the pointer across subsequent calls. */
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

/* Splitter for ae/repos/rev_io.ae's \n-separated names — kept here
 * because Aether can't hand C a fixed char[4][32] array directly. */
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

/* --- rep-cache access ---------------------------------------------- *
 *
 * Lookup helpers used by svnae_rep_read_blob (uncompressed size for
 * the inflate path) and svnae_rep_lookup_secondary (legacy NULL-on-
 * miss adapter for the few remaining char* callers in svnserver).
 * Both go through contrib.sqlite (rep_store.ae / rep_store_sec.ae). */

extern int aether_rep_cache_lookup_uncompressed(const char *repo, const char *sha);
extern const char *aether_rep_cache_sec_lookup(const char *repo,
                                                const char *primary,
                                                const char *algo);

/* Lookup: returns malloc'd hex or NULL. Caller frees. The Aether
 * helper returns "" on miss; we adapt at the boundary so existing
 * `if (shex)` callers keep working. */
char *
svnae_rep_lookup_secondary(const char *repo, const char *primary_hex,
                          const char *algo)
{
    const char *v = aether_rep_cache_sec_lookup(repo, primary_hex, algo);
    if (!v) return NULL;
    int n = (int)aether_string_length(v);
    if (n == 0) return NULL;
    const char *data = aether_string_data(v);
    char *out = malloc((size_t)n + 1);
    if (!out) return NULL;
    memcpy(out, data, (size_t)n);
    out[n] = '\0';
    return out;
}

/* --- public interface ---------------------------------------------- */

/* svnae_rep_write_blob — the dedup-aware blob-write orchestrator
 * lives entirely in ae/fs_fs/rep_store.ae::rep_write_blob now
 * (Round 105). This C-side name is preserved as a one-line forward
 * because some checked-in *_generated.c files (rebuild_generated.c
 * in particular — its source rebuild.ae no longer recompiles cleanly
 * under the current aetherc, so the cached .c is what links) still
 * reference the legacy svnae_ symbol. Once rebuild.ae regenerates,
 * this forward can go. */
extern const char *aether_rep_write_blob(const char *repo, const char *data, int len);
const char *
svnae_rep_write_blob(const char *repo, const char *data, int len)
{
    return aether_rep_write_blob(repo, data, len);
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

/* svnae_count_rep_files was a 1-line forward onto
 * aether_fs_count_rep_files (ae/fs_fs/count_reps.ae). Round 104
 * retired the wrapper — test_txn.ae now calls the aether_ name
 * directly. */
