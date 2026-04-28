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

/* fs_fs/shim.c — opaque holder for the test-suite tree builder.
 * Round 153 hoisted the (path, kind, content, content_len) edit
 * list to ae/fs_fs/tree_builder.ae using std.list / std.gintarr.
 * What stays here: 4 container handles + new/free + 4 getters +
 * 2 gintarr setters. Other historical contents (svnae_buf,
 * read_small_file, now_iso8601, sb_*) all retired in earlier
 * rounds. */

#include <stdlib.h>

extern void *aether_gintarr_new(int initial_cap);
extern void  aether_gintarr_free(void *a);
extern void *list_new(void);
extern void  list_free(void *list);

struct svnae_tree_builder {
    void *paths;
    void *kinds;
    void *contents;
    void *content_lens;
};

struct svnae_tree_builder *
svnae_fsfs_tree_builder_new(void)
{
    struct svnae_tree_builder *tb = calloc(1, sizeof *tb);
    if (!tb) return NULL;
    tb->paths        = list_new();
    tb->kinds        = aether_gintarr_new(16);
    tb->contents     = list_new();
    tb->content_lens = aether_gintarr_new(16);
    return tb;
}

void
svnae_fsfs_tree_builder_free(struct svnae_tree_builder *tb)
{
    if (!tb) return;
    list_free(tb->paths);
    aether_gintarr_free(tb->kinds);
    list_free(tb->contents);
    aether_gintarr_free(tb->content_lens);
    free(tb);
}

void *svnae_fsfs_tb_get_paths       (struct svnae_tree_builder *tb) { return tb ? tb->paths        : NULL; }
void *svnae_fsfs_tb_get_kinds       (struct svnae_tree_builder *tb) { return tb ? tb->kinds        : NULL; }
void *svnae_fsfs_tb_get_contents    (struct svnae_tree_builder *tb) { return tb ? tb->contents     : NULL; }
void *svnae_fsfs_tb_get_content_lens(struct svnae_tree_builder *tb) { return tb ? tb->content_lens : NULL; }

void svnae_fsfs_tb_set_kinds       (struct svnae_tree_builder *tb, void *p) { if (tb) tb->kinds        = p; }
void svnae_fsfs_tb_set_content_lens(struct svnae_tree_builder *tb, void *p) { if (tb) tb->content_lens = p; }
