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

/* ae/wc/status_shim.c — statuslist constructor + accessors and the
 * fnmatch(3) FFI helper. The list payload is a packed string built
 * Aether-side (status.ae): "<n>\x02<path>\x01<code>\x02...". The
 * struct + append + sort + per-row accessors that used to live here
 * are now packed_handle one-liners; the SQL drive and the FS walk
 * already lived in status.ae. */

#include <fnmatch.h>
#include <stdint.h>

#include "../subr/packed_handle/packed_handle.h"

/* Field-extractor extern declarations matching the Aether exports
 * in ae/wc/status_packed.ae. */
extern int         aether_wc_statuslist_count   (const char *packed);
extern const char *aether_wc_statuslist_path_at (const char *packed, int i);
extern int         aether_wc_statuslist_code_at (const char *packed, int i);

/* Construct from a Aether-built packed string. The Aether side
 * (status.ae) builds "<n>\x02<rec>\x02..." and calls this to wrap
 * into the shared handle type. */
struct svnae_wc_statuslist *
svnae_wc_statuslist_handle(const char *packed)
{
    if (!packed) return NULL;
    return (struct svnae_wc_statuslist *)svnae_packed_handle_new(packed,
                                            aether_wc_statuslist_count);
}

int svnae_wc_statuslist_count(const struct svnae_wc_statuslist *L) { return svnae_packed_count(L); }
const char *svnae_wc_statuslist_path(const struct svnae_wc_statuslist *L, int i) { return svnae_packed_pin_at((void *)L, i, aether_wc_statuslist_path_at); }
int  svnae_wc_statuslist_code(const struct svnae_wc_statuslist *L, int i) { return svnae_packed_int_at(L, i, aether_wc_statuslist_code_at); }
void svnae_wc_statuslist_free(struct svnae_wc_statuslist *L) { svnae_packed_handle_free((struct svnae_packed_handle *)L); }

/* FFI helper for ae/wc/ignore.ae — plain fnmatch(3) without
 * FNM_PATHNAME, matching svn:ignore's semantics (basename-style
 * patterns; `*` may span `/` characters if the caller ever passes a
 * multi-segment name). */
int32_t
svnae_fnmatch_plain(const char *glob, const char *path)
{
    return fnmatch(glob, path, 0) == 0 ? 1 : 0;
}
