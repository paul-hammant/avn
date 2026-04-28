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

/* ae/subr/rtree/shim.c — opaque holder for the remote-tree edit
 * list. Round 152 hoisted the storage bulk (struct array of
 * {path, kind, sha, data, data_len}) into ae/subr/rtree/rtree.ae
 * using three std.list (paths/shas/contents) and two std.gintarr
 * (kinds/content_lens). What stays here:
 *
 *   - struct svnae_rt (5 container handles)
 *   - svnae_rtree_new / _free
 *   - 5 get_*_(rt) accessors so the Aether helpers can reach in
 *   - 2 set_*_(rt, p) setters for the gintarr handles (gintarr_add
 *     can return a new handle on regrow). */

#include <stdlib.h>

#include "rtree.h"

extern void *aether_gintarr_new(int initial_cap);
extern void  aether_gintarr_free(void *a);
extern void *list_new(void);
extern void  list_free(void *list);

struct svnae_rt {
    void *kinds;
    void *paths;
    void *shas;
    void *contents;
    void *content_lens;
};

struct svnae_rt *
svnae_rtree_new(void)
{
    struct svnae_rt *rt = calloc(1, sizeof *rt);
    if (!rt) return NULL;
    rt->kinds        = aether_gintarr_new(16);
    rt->paths        = list_new();
    rt->shas         = list_new();
    rt->contents     = list_new();
    rt->content_lens = aether_gintarr_new(16);
    return rt;
}

void
svnae_rtree_free(struct svnae_rt *rt)
{
    if (!rt) return;
    aether_gintarr_free(rt->kinds);
    list_free(rt->paths);
    list_free(rt->shas);
    list_free(rt->contents);
    aether_gintarr_free(rt->content_lens);
    free(rt);
}

/* Container getters (Aether reaches in via these). */
void *svnae_rt_get_kinds       (struct svnae_rt *rt) { return rt ? rt->kinds        : NULL; }
void *svnae_rt_get_paths       (struct svnae_rt *rt) { return rt ? rt->paths        : NULL; }
void *svnae_rt_get_shas        (struct svnae_rt *rt) { return rt ? rt->shas         : NULL; }
void *svnae_rt_get_contents    (struct svnae_rt *rt) { return rt ? rt->contents     : NULL; }
void *svnae_rt_get_content_lens(struct svnae_rt *rt) { return rt ? rt->content_lens : NULL; }

/* gintarr handle setters (gintarr_add can regrow → new handle). */
void svnae_rt_set_kinds       (struct svnae_rt *rt, void *p) { if (rt) rt->kinds        = p; }
void svnae_rt_set_content_lens(struct svnae_rt *rt, void *p) { if (rt) rt->content_lens = p; }
