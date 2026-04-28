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

/* ae/wc/props_shim.c — proplist constructor + accessors (all
 * packed_handle-backed). The struct + new + append + per-row
 * accessors that used to live here are now packed_handle
 * one-liners; the SELECT loop in props.ae builds the packed
 * string directly. */

#include "../subr/packed_handle/packed_handle.h"

/* Field-extractor extern declarations matching the Aether exports
 * in ae/wc/props_packed.ae. */
extern int         aether_wc_proplist_count   (const char *packed);
extern const char *aether_wc_proplist_name_at (const char *packed, int i);
extern const char *aether_wc_proplist_value_at(const char *packed, int i);

struct svnae_wc_proplist *
svnae_wc_proplist_handle(const char *packed)
{
    if (!packed) return NULL;
    return (struct svnae_wc_proplist *)svnae_packed_handle_new(packed,
                                            aether_wc_proplist_count);
}

int svnae_wc_proplist_count(const struct svnae_wc_proplist *L) { return svnae_packed_count(L); }
const char *svnae_wc_proplist_name (const struct svnae_wc_proplist *L, int i) { return svnae_packed_pin_at((void *)L, i, aether_wc_proplist_name_at); }
const char *svnae_wc_proplist_value(const struct svnae_wc_proplist *L, int i) { return svnae_packed_pin_at((void *)L, i, aether_wc_proplist_value_at); }
void svnae_wc_proplist_free(struct svnae_wc_proplist *L) { svnae_packed_handle_free((struct svnae_packed_handle *)L); }

/* svnae_wc_propget + svnae_wc_props_free retired in Round 133 —
 * the strdup-detach wrapper was unnecessary once #297 made
 * AetherString returns length-aware on the .ae caller side. The
 * sole caller (svn/main.ae) now calls aether_wc_propget_value
 * directly and uses string.length() == 0 for the miss check. */
