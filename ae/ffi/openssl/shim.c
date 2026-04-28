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

/* ae/ffi/openssl/shim.c — thin glue to std.cryptography v2.
 *
 * Digest + Base64 work lives in ae/ffi/openssl/crypto_helpers.ae;
 * the C functions below preserve the historical svnae_openssl_*
 * signatures (six downstream shims + the test harness link
 * against them), copying AetherString results into malloc'd /
 * stack-buffer shapes the C contract expects. */

#include <stdlib.h>
#include <string.h>
#include "aether_string.h"   /* aether_string_data / aether_string_length */

extern const char *svnae_crypto_hash_hex(const char *algo, const char *data, int length);
extern int         svnae_crypto_hash_supported(const char *algo);
extern const char *svnae_crypto_b64_encode(const char *data, int length);
extern const char *svnae_crypto_b64_decode_capture(const char *b64);

/* TLS slot the Aether-side b64 decoder writes into so the second
 * accessor (length) can read back what the first accessor produced.
 * Same split-accessor shape std.fs.read_binary uses. */
static __thread int s_b64_decode_len = 0;

void svnae_crypto_b64_decode_set_len(int n) { s_b64_decode_len = n; }
int  svnae_crypto_b64_decode_len(void) { return s_b64_decode_len; }

/* --- public API ------------------------------------------------------- */

/* svnae_openssl_hash_hex_into / _hash_hex retired in Round 134:
 * the only external caller (svnserver_hash_hex) was retired in
 * the same round; other code paths reach svnae_crypto_hash_hex
 * (Aether-native, returns AetherString) directly. */

/* Base64-encode `src[0..len]` into a malloc'd NUL-terminated string.
 * Caller frees. Padded output — std.cryptography emits unpadded so
 * we append '=' as needed to make the length a multiple of 4. */
char *
svnae_openssl_b64_encode(const unsigned char *src, int len)
{
    const char *enc = svnae_crypto_b64_encode((const char *)src, len);
    if (!enc) return NULL;
    int n = (int)aether_string_length(enc);
    /* Pad to a multiple of 4 with '='. The downstream svn shims
     * expected padded output (matches reference RFC 4648 §4 and
     * the libcurl/openssl convention). */
    int pad = (4 - (n & 3)) & 3;
    char *out = malloc((size_t)n + (size_t)pad + 1);
    if (!out) return NULL;
    memcpy(out, aether_string_data(enc), (size_t)n);
    for (int i = 0; i < pad; i++) out[n + i] = '=';
    out[n + pad] = '\0';
    return out;
}

/* svnae_openssl_b64_decode (the malloc-detach (out, out_len) shape)
 * retired in Round 110. The svnserver commit-parse path calls
 * svnae_crypto_b64_decode_capture + _len directly from Aether, then
 * hands the AetherString into svnae_txn_add_file (the _aether
 * trampoline went away in Round 123 once #297 auto-unwrap landed). */

/* Is `algo` allowed as a repo's *content-address* algorithm? 1 = yes,
 * 0 = no. sha1 + sha256 only — the only two algorithms anything in
 * the port has ever asked for. std.cryptography.hash_supported
 * accepts any name OpenSSL recognises (sha384/sha512/sha3-*); we
 * narrow to our golden list here so admin commands can't install
 * anything else as primary/secondary on a real repo. */
/* svnae_openssl_hash_supported / svnae_openssl_hash_hex_len moved
 * to ae/ffi/openssl/crypto_helpers.ae in Round 106 — they were
 * pure string-comparison constant lookups with no openssl content. */
