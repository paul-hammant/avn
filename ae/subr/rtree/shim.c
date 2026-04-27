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

/* ae/subr/rtree/shim.c — single canonical implementation of the
 * remote-tree storage shape. Used by both the update walker
 * (ae/wc/update_shim.c forwards svnae_rtree_*) and the merge
 * walker (ae/wc/merge_shim.c forwards svnae_mergert_*). */

#include <stdlib.h>
#include <string.h>

#include "rtree.h"

extern int svnae_wc_hash_bytes(const char *wc_root, const char *data,
                               int len, char *out);

struct svnae_rt *
svnae_rt_new(void)
{
    return calloc(1, sizeof(struct svnae_rt));
}

static void
rt_grow(struct svnae_rt *rt)
{
    int nc = rt->cap ? rt->cap * 2 : 16;
    rt->items = realloc(rt->items, (size_t)nc * sizeof *rt->items);
    rt->cap = nc;
}

void
svnae_rt_add_dir(struct svnae_rt *rt, const char *path)
{
    if (!rt) return;
    if (rt->n == rt->cap) rt_grow(rt);
    struct svnae_rt_node *e = &rt->items[rt->n++];
    e->path = strdup(path ? path : "");
    e->kind = 1;
    e->sha1[0] = '\0';
    e->data = NULL;
    e->data_len = 0;
}

void
svnae_rt_add_file(struct svnae_rt *rt, const char *path,
                  const char *data, int data_len,
                  const char *wc_root)
{
    if (!rt) return;
    if (rt->n == rt->cap) rt_grow(rt);
    struct svnae_rt_node *e = &rt->items[rt->n++];
    e->path = strdup(path ? path : "");
    e->kind = 0;
    e->sha1[0] = '\0';
    if (data) {
        e->data = malloc((size_t)data_len + 1);
        memcpy(e->data, data, (size_t)data_len);
        e->data[data_len] = '\0';
        e->data_len = data_len;
        if (wc_root) svnae_wc_hash_bytes(wc_root, data, data_len, e->sha1);
    } else {
        e->data = NULL;
        e->data_len = 0;
    }
}

void
svnae_rt_free(struct svnae_rt *rt)
{
    if (!rt) return;
    for (int i = 0; i < rt->n; i++) {
        free(rt->items[i].path);
        free(rt->items[i].data);
    }
    free(rt->items);
    free(rt);
}

int svnae_rt_count(const struct svnae_rt *rt) { return rt ? rt->n : 0; }

const char *
svnae_rt_path_at(const struct svnae_rt *rt, int i)
{
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].path : "";
}

int
svnae_rt_kind_at(const struct svnae_rt *rt, int i)
{
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].kind : -1;
}

const char *
svnae_rt_sha_at(const struct svnae_rt *rt, int i)
{
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].sha1 : "";
}

const char *
svnae_rt_data_at(const struct svnae_rt *rt, int i)
{
    return (rt && i >= 0 && i < rt->n && rt->items[i].data) ? rt->items[i].data : "";
}

int
svnae_rt_data_len_at(const struct svnae_rt *rt, int i)
{
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].data_len : 0;
}

int
svnae_rt_find_by_path(const struct svnae_rt *rt, const char *path)
{
    if (!rt || !path) return -1;
    for (int i = 0; i < rt->n; i++) {
        if (strcmp(rt->items[i].path, path) == 0) return i;
    }
    return -1;
}
