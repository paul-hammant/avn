/*
 * Copyright 2026 Paul Hammant (portions).
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

/* pin_list — per-handle strdup'd string management.
 *
 * Used by RA and repos shims to hold returned string copies alive
 * across recursive / interleaved calls. Solves the stable-pointer
 * contract that C callers expect — without it, two concurrent accessor
 * calls on the same handle (e.g. name() then size()) that share TLS
 * buffers would corrupt each other.
 *
 * Pattern: struct contains {char *packed; int n; struct pin_list pins;}
 * Accessor functions strdup through pin_str, releasing pins_free on
 * handle destruction.
 */

#ifndef SVN_PIN_LIST_H
#define SVN_PIN_LIST_H

#include <stdlib.h>
#include <string.h>

struct pin_list {
    char **items;
    int n, cap;
};

static inline const char *
pin_str(struct pin_list *pl, const char *fresh)
{
    if (!fresh) fresh = "";
    if (pl->n == pl->cap) {
        int nc = pl->cap ? pl->cap * 2 : 8;
        char **np = realloc(pl->items, (size_t)nc * sizeof *np);
        if (!np) return "";
        pl->items = np;
        pl->cap = nc;
    }
    char *copy = strdup(fresh);
    if (!copy) return "";
    pl->items[pl->n++] = copy;
    return copy;
}

static inline void
pin_list_free(struct pin_list *pl)
{
    for (int i = 0; i < pl->n; i++) free(pl->items[i]);
    free(pl->items);
    pl->items = NULL;
    pl->n = pl->cap = 0;
}

#endif /* SVN_PIN_LIST_H */
