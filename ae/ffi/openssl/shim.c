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
 * Round 50 moved the EVP plumbing out of this file: the digest +
 * Base64 work now lives in ae/ffi/openssl/crypto_helpers.ae which
 * uses std.cryptography. The C functions below keep their existing
 * svnae_openssl_* signatures so every downstream caller (six
 * shims + the test harness) links unchanged — the bodies are now
 * thin wrappers that copy AetherString-backed results into the
 * malloc'd / stack-buffer shapes the C contract has always had.
 */

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

/* --- public API ------------------------------------------------------- */

/* Compute the named hash of `data[0..len]`, write lowercase hex to `out`
 * (caller-sized, typically 65 bytes for sha256 + NUL). Returns hex
 * length on success, 0 on unsupported algo or buffer too small.
 *
 * The "into a caller-provided buffer" shape matches the five inlined
 * EVP loops that existed in the port's C shims — they all wrote into
 * a stack char[65]. */
int
svnae_openssl_hash_hex_into(const char *algo, const char *data, int len, char *out)
{
    const char *hex = svnae_crypto_hash_hex(algo, data, len);
    if (!hex) { out[0] = '\0'; return 0; }
    int hlen = (int)aether_string_length(hex);
    if (hlen == 0 || hlen >= 65) { out[0] = '\0'; return 0; }
    const char *hdata = aether_string_data(hex);
    memcpy(out, hdata, (size_t)hlen);
    out[hlen] = '\0';
    return hlen;
}

/* Compute the named hash, return a malloc'd lowercase-hex string.
 * Caller frees with free(). Returns NULL on unsupported algo or OOM.
 *
 * This is the shape the subr/checksum module exposes to Aether tests
 * and to the high-level svnae_* APIs that want a returnable string. */
char *
svnae_openssl_hash_hex(const char *algo, const char *data, int len)
{
    char buf[65];
    int n = svnae_openssl_hash_hex_into(algo, data, len, buf);
    if (n == 0) return NULL;
    char *out = malloc((size_t)n + 1);
    if (!out) return NULL;
    memcpy(out, buf, (size_t)n + 1);
    return out;
}

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

/* Base64-decode `src[0..src_len]` into a malloc'd buffer. Returns 0 on
 * success with `*out` / `*out_len` set; caller frees with free().
 * Returns -1 on OOM or decode failure. */
int
svnae_openssl_b64_decode(const char *src, int src_len,
                        unsigned char **out, int *out_len)
{
    /* std.cryptography.base64_decode takes a NUL-terminated string,
     * not (ptr, len). If src isn't already NUL-terminated at src_len
     * we need to copy it. Cheap path: most callers pass strlen-sized
     * src, so check first. */
    char *tmp = NULL;
    const char *b64 = src;
    if (src_len < 0) return -1;
    if (src[src_len] != '\0') {
        tmp = malloc((size_t)src_len + 1);
        if (!tmp) return -1;
        memcpy(tmp, src, (size_t)src_len);
        tmp[src_len] = '\0';
        b64 = tmp;
    }
    s_b64_decode_len = 0;
    const char *bytes = svnae_crypto_b64_decode_capture(b64);
    if (tmp) free(tmp);
    if (!bytes) return -1;
    int n = s_b64_decode_len;
    if (n < 0) return -1;
    unsigned char *buf = malloc((size_t)n + 1);
    if (!buf) return -1;
    if (n > 0) memcpy(buf, aether_string_data(bytes), (size_t)n);
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

/* Is `algo` allowed as a repo's *content-address* algorithm? 1 = yes,
 * 0 = no. sha1 + sha256 only — the only two algorithms anything in
 * the port has ever asked for. std.cryptography.hash_supported
 * accepts any name OpenSSL recognises (sha384/sha512/sha3-*); we
 * narrow to our golden list here so admin commands can't install
 * anything else as primary/secondary on a real repo. */
int
svnae_openssl_hash_supported(const char *algo)
{
    if (!algo) return 0;
    if (strcmp(algo, "sha1")   == 0) return 1;
    if (strcmp(algo, "sha256") == 0) return 1;
    return 0;
}

/* Hex width produced by `algo`. 40 (sha1), 64 (sha256); 0 for
 * unsupported. */
int
svnae_openssl_hash_hex_len(const char *algo)
{
    if (!algo) return 0;
    if (strcmp(algo, "sha1")   == 0) return 40;
    if (strcmp(algo, "sha256") == 0) return 64;
    return 0;
}
