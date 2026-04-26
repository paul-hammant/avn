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

/* ae/wc/status_shim.c — statuslist storage primitive + accessors.
 *
 * Round 68 (Gordian knot) ported the recursive svnae_wc_status walk
 * to ae/wc/status.ae. The hand-rolled `strset` (linear-search string
 * set, ~25 lines) was retired in favour of a std.collections.map
 * tracked-set with O(1) membership probing. The svn:ignore prop
 * fetch + matching, the on-disk classification, and the recursive
 * unversioned walk all moved to Aether.
 *
 * What stays in C:
 *   - struct svnae_wc_statuslist + the (path, code) accessors
 *   - new() / append() / sort() the Aether walk calls per result row
 *   - svnae_fnmatch_plain  (POSIX fnmatch(3) FFI for ae/wc/ignore.ae)
 */

#include <fnmatch.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct status_entry { char *path; char code; };
struct svnae_wc_statuslist { struct status_entry *items; int n; int cap; };

struct svnae_wc_statuslist *
svnae_wc_statuslist_new(void)
{
    return calloc(1, sizeof(struct svnae_wc_statuslist));
}

int
svnae_wc_statuslist_append(struct svnae_wc_statuslist *L,
                            const char *path, int code)
{
    if (!L) return -1;
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 8;
        struct status_entry *p = realloc(L->items, (size_t)nc * sizeof *p);
        if (!p) return -1;
        L->items = p;
        L->cap = nc;
    }
    L->items[L->n].path = strdup(path ? path : "");
    L->items[L->n].code = (char)code;
    L->n++;
    return 0;
}

static int
entry_cmp(const void *a, const void *b)
{
    return strcmp(((const struct status_entry *)a)->path,
                  ((const struct status_entry *)b)->path);
}

void
svnae_wc_statuslist_sort(struct svnae_wc_statuslist *L)
{
    if (!L || L->n <= 1) return;
    qsort(L->items, (size_t)L->n, sizeof *L->items, entry_cmp);
}

int svnae_wc_statuslist_count(const struct svnae_wc_statuslist *L) { return L ? L->n : 0; }

const char *
svnae_wc_statuslist_path(const struct svnae_wc_statuslist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].path;
}

int
svnae_wc_statuslist_code(const struct svnae_wc_statuslist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return 0;
    return (int)(unsigned char)L->items[i].code;
}

void
svnae_wc_statuslist_free(struct svnae_wc_statuslist *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) free(L->items[i].path);
    free(L->items);
    free(L);
}

/* FFI helper for ae/wc/ignore.ae — plain fnmatch(3) without
 * FNM_PATHNAME, matching svn:ignore's semantics (basename-style
 * patterns; `*` may span `/` characters if the caller ever passes a
 * multi-segment name). */
int32_t
svnae_fnmatch_plain(const char *glob, const char *path)
{
    return fnmatch(glob, path, 0) == 0 ? 1 : 0;
}
