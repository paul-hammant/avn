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

/* ae/wc/props_shim.c — proplist storage primitive + free helpers.
 * Same shape as db_shim.c's nodelist:
 *   - struct svnae_wc_proplist + (name, value) accessors
 *   - new()/append() the Aether-side SELECT loop in props.ae calls
 *   - malloc-free trampoline for propget's returned string */

#include <stdlib.h>
#include <string.h>

struct prop_entry { char *name; char *value; };
struct svnae_wc_proplist { struct prop_entry *items; int n; int cap; };

struct svnae_wc_proplist *
svnae_wc_proplist_new(void)
{
    return calloc(1, sizeof(struct svnae_wc_proplist));
}

int
svnae_wc_proplist_append(struct svnae_wc_proplist *L,
                         const char *name, const char *value)
{
    if (!L) return -1;
    if (L->n == L->cap) {
        int nc = L->cap ? L->cap * 2 : 8;
        struct prop_entry *p = realloc(L->items, (size_t)nc * sizeof *p);
        if (!p) return -1;
        L->items = p;
        L->cap = nc;
    }
    L->items[L->n].name  = strdup(name  ? name  : "");
    L->items[L->n].value = strdup(value ? value : "");
    L->n++;
    return 0;
}

int svnae_wc_proplist_count(const struct svnae_wc_proplist *L) { return L ? L->n : 0; }

const char *
svnae_wc_proplist_name(const struct svnae_wc_proplist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].name;
}

const char *
svnae_wc_proplist_value(const struct svnae_wc_proplist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].value;
}

void
svnae_wc_proplist_free(struct svnae_wc_proplist *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) { free(L->items[i].name); free(L->items[i].value); }
    free(L->items);
    free(L);
}

/* Legacy svnae_wc_propget — strdup-into-caller-owned-heap wrapper
 * around the Aether-side aether_wc_propget_value (returns "" on
 * miss). Preserves the historical `char *` (+ NULL-on-miss + caller-
 * frees) ABI. Aether-side callers can use aether_wc_propget_value
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
