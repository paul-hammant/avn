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

/* ae/wc/merge_shim.c — public svnae_mergert_* + mergert_* ABI for
 * svn merge. The storage primitive lives in ae/subr/rtree/ and is
 * shared with ae/wc/update_shim.c (svnae_rtree_*). The TLS wc_root
 * pointer reused for sha1 computation also lives in update_shim.c
 * (svnae_update_get_wc_root). */

#include "../subr/rtree/rtree.h"

extern const char *svnae_update_get_wc_root(void);

/* Public svnae_mergert_* ABI — thin forwards onto svnae_rt_*. */
void *svnae_mergert_new (void)                       { return svnae_rt_new(); }
void  svnae_mergert_free(void *rt)                   { svnae_rt_free((struct svnae_rt *)rt); }
void  svnae_mergert_add_dir (void *rt, const char *path) { svnae_rt_add_dir((struct svnae_rt *)rt, path); }
void  svnae_mergert_add_file(void *rt, const char *path, const char *data, int data_len) { svnae_rt_add_file((struct svnae_rt *)rt, path, data, data_len, svnae_update_get_wc_root()); }

/* Aether-side reader names. */
int         mergert_count       (const struct svnae_rt *rt)                 { return svnae_rt_count(rt); }
const char *mergert_path_at     (const struct svnae_rt *rt, int i)          { return svnae_rt_path_at(rt, i); }
int         mergert_kind_at     (const struct svnae_rt *rt, int i)          { return svnae_rt_kind_at(rt, i); }
const char *mergert_sha_at      (const struct svnae_rt *rt, int i)          { return svnae_rt_sha_at(rt, i); }
const char *mergert_data_at     (const struct svnae_rt *rt, int i)          { return svnae_rt_data_at(rt, i); }
int         mergert_data_len_at (const struct svnae_rt *rt, int i)          { return svnae_rt_data_len_at(rt, i); }
int         mergert_find_by_path(const struct svnae_rt *rt, const char *p)  { return svnae_rt_find_by_path(rt, p); }
