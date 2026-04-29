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

/* rep_store_shim.c — thin malloc-detach over the Aether-side
 * rep_read_decoded + slice helper. Everything else (the encode/
 * write side, the on-disk format primitives, secondary-pair
 * lookup) is in ae/fs_fs/rep_store.ae and friends. */

#include <stdlib.h>
#include <string.h>

/* --- Aether bridge ---------------------------------------------------- */

/* Blob read lives in ae/fs_fs/rep_store.ae (std.zlib + std.fs). The
 * encode + write side moved entirely to Aether in Round 105 — see
 * rep_write_blob there. The decode side stays C-glued because the
 * read returns a length-aware AetherString that we copy into a
 * malloc'd, NUL-terminated buffer at the FFI boundary. */
extern const char *aether_rep_read_decoded(const char *repo, const char *sha,
                                            int uncompressed_size);

#include "aether_string.h"

/* aether_pristine_concat_binary_n moved to ae/subr/binbuf.ae in
 * Round 126 (std.bytes makes the binary-safe concat expressible
 * in Aether). aether_pristine_slice_binary stayed C-side because
 * the auto-unwrap from #297 made the .ae caller's `s` a raw
 * char* with no recoverable length. Round 163 ports it Aether-side
 * as aether_pristine_slice_binary_n(s, s_len, start, end) using
 * std.string.substring_n (which arrived in Aether [current] —
 * see std_remaining_gaps.md / string_substring_binary_NULs.md). */

/* svnae_repo_primary_hash retired in Round 132 — was a TLS-buf
 * detour over aether_repo_primary_hash. The Aether body already
 * has the "sha1" fallback on missing format files; the C-side
 * cache shape was unnecessary once #297 + length-aware AetherString
 * returns landed. */

/* svnae_repo_secondary_hashes (the char[4][32] splitter) +
 * svnae_rep_lookup_secondary (the malloc-detach lookup) retired in
 * Round 136 — both existed only to feed svnserver_build_secondary_pairs
 * a C-shaped array; the .ae caller now walks
 * aether_format_secondary_count / _hash + aether_rep_cache_sec_lookup
 * directly. */

/* --- rep-cache access ---------------------------------------------- */
extern int aether_rep_cache_lookup_uncompressed(const char *repo, const char *sha);

/* --- public interface ---------------------------------------------- */

/* svnae_rep_write_blob — the dedup-aware blob-write orchestrator
 * lives entirely in ae/fs_fs/rep_store.ae::rep_write_blob (Round
 * 105). The temporary svnae_-named forward we kept here for
 * rebuild_generated.c stale-cache compatibility was retired in
 * Round 107 once rebuild.ae regenerated cleanly under aether
 * 0.95+. */

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
 * svnae_rep_read_blob. (svnae_txn_rebuild_root retired in Round 135.) */
void svnae_rep_free(char *p) { free(p); }

/* svnae_count_rep_files was a 1-line forward onto
 * aether_fs_count_rep_files (ae/fs_fs/count_reps.ae). Round 104
 * retired the wrapper — test_txn.ae now calls the aether_ name
 * directly. */
