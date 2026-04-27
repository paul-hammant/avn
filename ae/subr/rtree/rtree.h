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

/* ae/subr/rtree/rtree.h — shared remote-tree storage shape used by
 * the update walker and the merge walker. Per-node binary content
 * (file payload + sha1 hex + path + kind) lives on the C side
 * because Aether strings can't safely carry blobs with embedded
 * NULs across FFI.
 *
 * Round 99 collapsed the duplicated structs in ae/wc/update_shim.c
 * (svnae_rtree) and ae/wc/merge_shim.c (svnae_mergert) — both held
 * the exact same {path, kind, sha1[65], data, data_len} record,
 * a growable items array, and the same _count/_path_at/_kind_at/
 * _sha_at/_data_at/_data_len_at/_find_by_path accessors. The
 * domain-specific public ABIs (svnae_rtree_*, svnae_mergert_*,
 * rtree_*, mergert_*) stay one-line forwards on top of this. */

#ifndef AE_SUBR_RTREE_H
#define AE_SUBR_RTREE_H

struct svnae_rt_node {
    char *path;       /* "a/b/c.txt" — no leading slash, '/' separator */
    int   kind;       /* 0 = file, 1 = dir */
    char  sha1[65];   /* hex digest, zero-length string when kind==1 */
    char *data;       /* malloc'd, length = data_len. NULL for dirs. */
    int   data_len;
};

struct svnae_rt {
    struct svnae_rt_node *items;
    int n, cap;
};

/* Lifecycle. */
struct svnae_rt *svnae_rt_new(void);
void             svnae_rt_free(struct svnae_rt *rt);

/* Add. wc_root is the WC pointer used to compute the sha1 via
 * svnae_wc_hash_bytes. NULL wc_root → sha1 stays empty. */
void svnae_rt_add_dir (struct svnae_rt *rt, const char *path);
void svnae_rt_add_file(struct svnae_rt *rt, const char *path,
                       const char *data, int data_len,
                       const char *wc_root);

/* Read. */
int         svnae_rt_count       (const struct svnae_rt *rt);
const char *svnae_rt_path_at     (const struct svnae_rt *rt, int i);
int         svnae_rt_kind_at     (const struct svnae_rt *rt, int i);
const char *svnae_rt_sha_at      (const struct svnae_rt *rt, int i);
const char *svnae_rt_data_at     (const struct svnae_rt *rt, int i);
int         svnae_rt_data_len_at (const struct svnae_rt *rt, int i);
int         svnae_rt_find_by_path(const struct svnae_rt *rt, const char *path);

#endif /* AE_SUBR_RTREE_H */
