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
 * Just public svnae_wc_* signatures — out-param + length-aware C
 * ABI shaping that adapts Aether's string returns for downstream
 * C callers. The byte-level helpers (concat / pack_le32) moved to
 * subr/binbuf.ae; slice stays in rep_store_shim.c (see binbuf.ae
 * for why). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aether_string.h"   /* aether_string_data / aether_string_length / string_new_with_length */

/* aether_pristine_pack_le32 / _concat_binary_n moved to
 * ae/subr/binbuf.ae (Round 126/127). aether_pristine_slice_binary
 * stays C-side in fs_fs/rep_store_shim.c — see binbuf.ae for why. */

extern const char *aether_wc_hash_bytes(const char *wc_root,
                                        const char *data, int length);
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

/* svnae_wc_hash_file retired in Round 130 — only caller was the
 * (also-retired) update_shim.c::update_sha1_of_file trampoline. */

/* svnae_wc_pristine_{has,size,put,get,free} all retired:
 *   - has / size: Round 104 — 1-line forwards, callers use aether_* directly
 *   - put: Round 129 — TLS-sha truncation never fired (sha1 is always 40
 *     chars), .ae callers now use aether_wc_pristine_put directly and
 *     receive an AetherString (refcount handles lifetime)
 *   - get + free: Round 129 — caller-side malloc detach was unnecessary
 *     once #297 + length-aware AetherString returns landed; callers
 *     dropped the explicit free() too. */
