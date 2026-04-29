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

/* ae/ffi/openssl/shim.c — fully retired in Round 160.
 *
 * The padded-base64 padding loop (the last live function) went
 * away when crypto_helpers.ae switched to
 * cryptography.base64_encode_padded — see std_cryptography_gaps.md
 * Gap 1 ("Optional padded base64_encode") which shipped in Aether
 * [current]. Every other historical openssl_* entry was retired in
 * earlier rounds (134 / 110 / 106 / 159). File kept as a headstone
 * so aether.toml's extra_sources reference doesn't dangle. */
