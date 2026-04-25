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

/* ae/wc/pristine_shim.c — thin adapter over ae/wc/pristine.ae.
 *
 * The pristine store (put / get / has / size / hash) is fully
 * ported to Aether now that std.cryptography (sha1/sha256) and
 * std.zlib (deflate/inflate) landed, and Round 37 moved the
 * wc.db info-table lookup for the configured hash algorithm to
 * Aether as well. What remains here:
 *
 *   1. Three small byte-level helpers — pack_le32, binary-safe
 *      concat, binary-safe slice — wrapping Aether's
 *      string_new_with_length / AetherString layout. std.string
 *      has no byte-construction primitive that doesn't intermediate
 *      through a NUL-terminated C-style string; doing these in C
 *      keeps the Aether implementation binary-safe.
 *
 *   2. Adapters for the public svnae_wc_* signatures — existing
 *      callers expect "out-param buffer with hex length", "NULL
 *      on miss", etc.; we marshal between those and the Aether
 *      wrappers' string returns.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- byte-level helpers for the Aether side ------------------------- *
 *
 * string_new_with_length takes a length-prefixed AetherString; we
 * use it to construct binary payloads with embedded NULs.
 */

#include "aether_string.h"   /* aether_string_data / aether_string_length / string_new_with_length */

/* 4-byte little-endian packing of `v`. Returns an AetherString of
 * length 4. */
const char *
aether_pristine_pack_le32(int v)
{
    char buf[4];
    buf[0] = (char)(v         & 0xff);
    buf[1] = (char)((v >> 8)  & 0xff);
    buf[2] = (char)((v >> 16) & 0xff);
    buf[3] = (char)((v >> 24) & 0xff);
    return (const char *)string_new_with_length(buf, 4);
}

/* aether_pristine_concat_binary / aether_pristine_slice_binary live
 * in ae/fs_fs/rep_store_shim.c now — the fs_fs rep-store port
 * (round 31) needed them too, and both shims are linked into every
 * binary that uses either pristine_generated.c or
 * rep_store_generated.c. Exporting once from rep_store_shim.c
 * avoids duplicate-symbol link errors. */

/* --- public API — thin C adapters over the Aether wrappers --------- *
 *
 * The svnae_wc_* signatures are what existing callers (checkout,
 * update, merge, revert, status, verify) expect; we marshal to /
 * from the Aether-side string returns.
 */

extern const char *aether_wc_hash_bytes(const char *wc_root,
                                        const char *data, int length);
extern const char *aether_wc_hash_file(const char *wc_root, const char *path);
extern const char *aether_wc_pristine_put(const char *wc_root,
                                           const char *data, int length);
extern const char *aether_wc_pristine_get(const char *wc_root, const char *sha);
extern int         aether_wc_pristine_has(const char *wc_root, const char *sha);
extern int         aether_wc_pristine_size(const char *wc_root, const char *sha);

/* Hash `data[0..len]` under the WC's algorithm. Copy into caller's
 * `out` buffer (>= 65 bytes) and return the hex length. 0 on
 * failure. */
int
svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out)
{
    const char *hex = aether_wc_hash_bytes(wc_root, data, len);
    if (!hex) { out[0] = '\0'; return 0; }
    const char *hdata = aether_string_data(hex);
    int         hlen  = (int)aether_string_length(hex);
    if (hlen >= 65) hlen = 64;
    memcpy(out, hdata, (size_t)hlen);
    out[hlen] = '\0';
    return hlen;
}

int
svnae_wc_hash_file(const char *wc_root, const char *path, char *out)
{
    const char *hex = aether_wc_hash_file(wc_root, path);
    if (!hex) { out[0] = '\0'; return -1; }
    const char *hdata = aether_string_data(hex);
    int         hlen  = (int)aether_string_length(hex);
    if (hlen == 0) { out[0] = '\0'; return -1; }
    if (hlen >= 65) hlen = 64;
    memcpy(out, hdata, (size_t)hlen);
    out[hlen] = '\0';
    return 0;
}

/* Put. Returns the sha hex (TLS-cached; caller copies before next
 * call) or NULL on failure. */
/* Ephemeral trace that exposed an OpenSSL stub path: when the build
 * didn't set -DAETHER_HAS_OPENSSL, std.cryptography returned NULL
 * digests and downstream code read past a NULL pointer. Kept as a
 * comment so I don't reach for the same trace a third time. */
const char *
svnae_wc_pristine_put(const char *wc_root, const char *data, int len)
{
    static __thread char sha[65];
    const char *r = aether_wc_pristine_put(wc_root, data, len);
    if (!r) return NULL;
    const char *rdata = aether_string_data(r);
    int         rlen  = (int)aether_string_length(r);
    if (rlen == 0) return NULL;
    if (rlen >= 65) rlen = 64;
    memcpy(sha, rdata, (size_t)rlen);
    sha[rlen] = '\0';
    return sha;
}

/* Get. Returns a malloc'd NUL-terminated copy (embedded NULs
 * preserved up to the recorded uncompressed length, but length
 * isn't exposed — existing callers either know the content is
 * text, or query svnae_wc_pristine_size first). NULL on miss. */
char *
svnae_wc_pristine_get(const char *wc_root, const char *sha)
{
    const char *r = aether_wc_pristine_get(wc_root, sha);
    if (!r) return NULL;
    const char *rdata = aether_string_data(r);
    int         rlen  = (int)aether_string_length(r);
    if (rlen == 0) {
        /* Empty payload is a miss — pristine entries are never empty:
         * every real blob has at least one byte in it. (Matches C
         * version's behaviour, whose uncompressed=0 was never emitted
         * because svnae_rep_write_blob rejects len==0 too.) */
        return NULL;
    }
    char *out = malloc((size_t)rlen + 1);
    if (!out) return NULL;
    memcpy(out, rdata, (size_t)rlen);
    out[rlen] = '\0';
    return out;
}

int
svnae_wc_pristine_has(const char *wc_root, const char *sha)
{
    return aether_wc_pristine_has(wc_root, sha);
}

int
svnae_wc_pristine_size(const char *wc_root, const char *sha)
{
    return aether_wc_pristine_size(wc_root, sha);
}

void svnae_wc_pristine_free(char *p) { free(p); }
