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

/* checksum/shim.c — Aether-facing hash API.
 *
 * All the EVP plumbing now lives in ae/ffi/openssl/shim.c. This file
 * keeps the svnae_* names the rest of the port already uses, as thin
 * forwards to the consolidated entries.
 */

#include <stdlib.h>

/* Consolidated in ae/ffi/openssl/shim.c. */
extern char *svnae_openssl_hash_hex(const char *algo, const char *data, int len);
extern int   svnae_openssl_hash_hex_len(const char *algo);
extern int   svnae_openssl_hash_supported(const char *algo);

char *svnae_sha1_hex(const char *data, int data_len)   { return svnae_openssl_hash_hex("sha1",   data, data_len); }
char *svnae_sha256_hex(const char *data, int data_len) { return svnae_openssl_hash_hex("sha256", data, data_len); }

char *
svnae_hash_hex(const char *algo, const char *data, int data_len)
{
    return svnae_openssl_hash_hex(algo, data, data_len);
}

int
svnae_hash_hex_len(const char *algo)
{
    return svnae_openssl_hash_hex_len(algo);
}

int
svnae_hash_supported(const char *algo)
{
    return svnae_openssl_hash_supported(algo);
}

void svnae_checksum_free_hex(char *s) { free(s); }
