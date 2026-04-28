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

/* ae/wc/props_shim.c — fully retired.
 *
 * Round 133 retired svnae_wc_propget + svnae_wc_props_free.
 * Round 157 retired svnae_wc_proplist_handle + accessors —
 * moved to ae/wc/wc_accessors.ae. The .c file is kept as a
 * headstone so aether.toml's extra_sources reference doesn't
 * dangle (removing it cleanly is a follow-up bookkeeping
 * round). */
