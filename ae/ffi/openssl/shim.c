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

/* ae/ffi/openssl/shim.c — consolidated EVP digest + base64 wrappers.
 *
 * Before this file existed, the EVP-Init/Update/Final + hex-encode
 * sequence was duplicated in six places (checksum, rep_store, pristine
 * x2, svnserver x2, ra). All duplicates now call through to the three
 * entries declared here.
 *
 * Golden list for svn-aether: sha1, sha256. Everything else returns
 * NULL / 0. Adding another algorithm here is the only code change
 * required to extend the server's --algos support.
 *
 * This file is the landing zone for future openssl FFI — TLS support
 * (Phase 0 item 5) and password-hashing (when auth lands) will grow
 * here next, sharing the same link line (-lssl -lcrypto).
 */

#include <stdlib.h>
#include <string.h>
#include <openssl/evp.h>

/* --- internal helpers ------------------------------------------------- */

static const EVP_MD *
evp_by_name(const char *name)
{
    if (!name) return NULL;
    if (strcmp(name, "sha1")   == 0) return EVP_sha1();
    if (strcmp(name, "sha256") == 0) return EVP_sha256();
    return NULL;
}

static int
hex_encode_into(const unsigned char *bytes, unsigned int n, char *out, size_t out_sz)
{
    if ((size_t)n * 2 + 1 > out_sz) return -1;
    static const char hex[] = "0123456789abcdef";
    for (unsigned int i = 0; i < n; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
    return (int)n * 2;
}

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
    const EVP_MD *md = evp_by_name(algo);
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
    return hex_encode_into(dig, dlen, out, 65);
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
 * Caller frees. Padded output — the exact byte count EVP_EncodeBlock
 * returns is 4*((len+2)/3). */
char *
svnae_openssl_b64_encode(const unsigned char *src, int len)
{
    int out_cap = 4 * ((len + 2) / 3) + 1;
    char *out = malloc((size_t)out_cap);
    if (!out) return NULL;
    int n = EVP_EncodeBlock((unsigned char *)out, src, len);
    if (n < 0) { free(out); return NULL; }
    out[n] = '\0';
    return out;
}

/* Base64-decode `src[0..src_len]` into a malloc'd buffer. Returns 0 on
 * success with `*out` / `*out_len` set; caller frees with free().
 * Returns -1 on OOM or decode failure. OpenSSL's EVP_DecodeBlock is
 * padding-aware enough that we strip trailing '=' and compute the
 * true length ourselves. */
int
svnae_openssl_b64_decode(const char *src, int src_len,
                        unsigned char **out, int *out_len)
{
    int raw_len = src_len;
    int pad = 0;
    while (raw_len > 0 && src[raw_len - 1] == '=') { raw_len--; pad++; }
    int expected = (src_len / 4) * 3 - pad;
    unsigned char *buf = malloc((size_t)expected + 1);
    if (!buf) return -1;
    int n = EVP_DecodeBlock(buf, (const unsigned char *)src, src_len);
    if (n < 0) { free(buf); return -1; }
    n -= pad;
    if (n < 0) n = 0;
    buf[n] = '\0';
    *out = buf;
    *out_len = n;
    return 0;
}

/* Is `algo` allowed as a repo's *content-address* algorithm? 1 = yes,
 * 0 = no. sha1 + sha256 only — the only two algorithms anything in
 * the port has ever asked for. */
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
