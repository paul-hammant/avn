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

/* ae/wc/props_shim.c — proplist constructor + accessors (packed_
 * handle-backed) plus the legacy svnae_wc_propget malloc-detach
 * trampoline. The struct + new + append + per-row accessors that
 * used to live here are now packed_handle one-liners; the SELECT
 * loop in props.ae builds the packed string directly. */

#include <stdlib.h>
#include <string.h>

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

/* Legacy svnae_wc_propget — strdup-into-caller-owned-heap wrapper
 * around the Aether-side aether_wc_propget_value (returns "" on
 * miss). Preserves the historical `char *` (+ NULL-on-miss + caller-
 * frees) ABI. Aether-side callers use aether_wc_propget_value
 * directly. */
extern const char *aether_wc_propget_value(const char *wc_root,
                                            const char *path,
                                            const char *name);
char *
svnae_wc_propget(const char *wc_root, const char *path, const char *name)
{
    const char *v = aether_wc_propget_value(wc_root, path, name);
    if (!v || !*v) return NULL;
    return strdup(v);
}

void svnae_wc_props_free(char *s) { free(s); }
