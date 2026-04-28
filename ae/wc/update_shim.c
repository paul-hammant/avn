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

/* ae/wc/update_shim.c — public svnae_rtree_* + rtree_* ABI for svn
 * update. The storage primitive lives in ae/subr/rtree/ and is
 * shared with ae/wc/merge_shim.c (svnae_mergert_*). */

#include "../subr/rtree/rtree.h"

/* Public svnae_rtree_* ABI — thin forwards onto svnae_rt_*. The
 * Aether walker holds a `void *` rt handle and calls these names.
 * Round 140 retired the TLS g_wc_root: add_file now takes wc_root
 * explicitly so the .ae walker threads it through directly instead
 * of going through a C-side global. */
void *svnae_rtree_new (void)                     { return svnae_rt_new(); }
void  svnae_rtree_free(void *rt)                 { svnae_rt_free((struct svnae_rt *)rt); }
void  svnae_rtree_add_dir (void *rt, const char *path) { svnae_rt_add_dir((struct svnae_rt *)rt, path); }
void  svnae_rtree_add_file(void *rt, const char *path, const char *data, int data_len, const char *wc_root) { svnae_rt_add_file((struct svnae_rt *)rt, path, data, data_len, wc_root); }

/* Aether-side reader names (no svnae_ prefix — historical). */
int         rtree_count       (const struct svnae_rt *rt)                 { return svnae_rt_count(rt); }
const char *rtree_path_at     (const struct svnae_rt *rt, int i)          { return svnae_rt_path_at(rt, i); }
int         rtree_kind_at     (const struct svnae_rt *rt, int i)          { return svnae_rt_kind_at(rt, i); }
const char *rtree_sha_at      (const struct svnae_rt *rt, int i)          { return svnae_rt_sha_at(rt, i); }
const char *rtree_data_at     (const struct svnae_rt *rt, int i)          { return svnae_rt_data_at(rt, i); }
int         rtree_data_len_at (const struct svnae_rt *rt, int i)          { return svnae_rt_data_len_at(rt, i); }
int         rtree_find_by_path(const struct svnae_rt *rt, const char *p)  { return svnae_rt_find_by_path(rt, p); }

/* update_sha1_of_file retired in Round 130 — was a thin TLS-buf
 * detour over svnae_wc_hash_file. update_apply.ae now calls the
 * canonical aether_wc_hash_file directly. */
