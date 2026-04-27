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
 * Two responsibilities:
 *  1. byte-level helpers (pack_le32; concat/slice live in
 *     rep_store_shim.c which is always co-linked) wrapping
 *     string_new_with_length so Aether can build binary payloads.
 *  2. public svnae_wc_* signatures — out-param/length-aware C ABI —
 *     adapting Aether's string returns. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* aether_pristine_concat_binary / _slice_binary live in
 * fs_fs/rep_store_shim.c — both shims always co-link, so exporting
 * once avoids duplicate-symbol errors. */

extern const char *aether_wc_hash_bytes(const char *wc_root,
                                        const char *data, int length);
extern const char *aether_wc_hash_file(const char *wc_root, const char *path);
extern const char *aether_wc_pristine_put(const char *wc_root,
                                           const char *data, int length);
extern const char *aether_wc_pristine_get(const char *wc_root, const char *sha);

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

/* svnae_wc_pristine_has / _size were 1-line forwards onto
 * aether_wc_pristine_has / _size; Round 104 retired them in favour
 * of letting .ae callers use the aether_* names directly. The C
 * pristine_get and pristine_put wrappers stay because they do
 * length-aware malloc detach + TLS-cached sha return. */

void svnae_wc_pristine_free(char *p) { free(p); }
