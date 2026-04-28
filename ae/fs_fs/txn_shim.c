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

/* fs_fs/txn_shim.c — opaque holder for the transaction edit list.
 *
 * Round 151 hoisted the edit-list bulk into ae/fs_fs/txn.ae using
 * three std.list (paths/contents/copy_shas) and three std.gintarr
 * (kinds/content_lens/copy_kinds). What stays here:
 *
 *   - struct svnae_txn (base_rev + 6 container handles)
 *   - svnae_txn_new / _free / _base_rev
 *   - 6 get_*_(t) accessors so the Aether helpers can reach the
 *     containers
 *   - 3 set_*_(t, p) setters for the gintarr handles (gintarr_add
 *     can return a new handle on regrow; the .ae caller threads
 *     it back through the setter)
 */

#include <stdlib.h>

extern void *aether_gintarr_new(int initial_cap);
extern void  aether_gintarr_free(void *a);
extern void *list_new(void);
extern void  list_free(void *list);

struct svnae_txn {
    int   base_rev;
    void *kinds;
    void *paths;
    void *contents;
    void *content_lens;
    void *copy_shas;
    void *copy_kinds;
};

struct svnae_txn *
svnae_txn_new(int base_rev)
{
    struct svnae_txn *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->base_rev     = base_rev;
    t->kinds        = aether_gintarr_new(8);
    t->paths        = list_new();
    t->contents     = list_new();
    t->content_lens = aether_gintarr_new(8);
    t->copy_shas    = list_new();
    t->copy_kinds   = aether_gintarr_new(8);
    return t;
}

int svnae_txn_base_rev(const struct svnae_txn *t) { return t ? t->base_rev : -1; }

void
svnae_txn_free(struct svnae_txn *t)
{
    if (!t) return;
    aether_gintarr_free(t->kinds);
    list_free(t->paths);
    list_free(t->contents);
    aether_gintarr_free(t->content_lens);
    list_free(t->copy_shas);
    aether_gintarr_free(t->copy_kinds);
    free(t);
}

/* Container getters — Aether helpers in txn.ae reach in via these. */
void *svnae_txn_get_kinds       (struct svnae_txn *t) { return t ? t->kinds        : NULL; }
void *svnae_txn_get_paths       (struct svnae_txn *t) { return t ? t->paths        : NULL; }
void *svnae_txn_get_contents    (struct svnae_txn *t) { return t ? t->contents     : NULL; }
void *svnae_txn_get_content_lens(struct svnae_txn *t) { return t ? t->content_lens : NULL; }
void *svnae_txn_get_copy_shas   (struct svnae_txn *t) { return t ? t->copy_shas    : NULL; }
void *svnae_txn_get_copy_kinds  (struct svnae_txn *t) { return t ? t->copy_kinds   : NULL; }

/* Setters for the three gintarr containers — gintarr_add can
 * regrow and return a new handle; the .ae caller threads it back
 * through here so subsequent reads see the new buffer. */
void svnae_txn_set_kinds       (struct svnae_txn *t, void *p) { if (t) t->kinds        = p; }
void svnae_txn_set_content_lens(struct svnae_txn *t, void *p) { if (t) t->content_lens = p; }
void svnae_txn_set_copy_kinds  (struct svnae_txn *t, void *p) { if (t) t->copy_kinds   = p; }
