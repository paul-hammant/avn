/*
 * Copyright 2026 Paul Hammant (portions).
 * Portions copyright Apache Subversion project contributors (2001-2026).
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

/* ae/subr/packed_handle/shim.c — fully retired in Round 157.
 *
 * The shared svnae_packed_handle (struct + pin_list) backed every
 * accessor family in ra/repos/wc shims. Rounds 154–157 ported
 * those families to .ae files where the "handle" is just the
 * underlying packed AetherString — Aether's refcount upholds the
 * stable-pointer contract that pin_str provided.
 *
 * Kept as a headstone so aether.toml's extra_sources reference
 * doesn't dangle (removing it cleanly is a follow-up bookkeeping
 * round). */
