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

/* ae/wc/merge_shim.c — opaque rtree storage for svn merge.
 *
 * Per-node binary content stored on the C side because Aether
 * strings can't safely carry blobs with embedded NULs across FFI.
 * Walker in merge_walk.ae, two-pass apply in merge_apply.ae,
 * mergeinfo arithmetic in mergeinfo.ae, top-level orchestration
 * in merge.ae. The TLS wc_root pointer is shared with
 * update_shim.c (same set/get helpers). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out);

/* TLS wc_root pointer + setter live in update_shim.c; both files
 * read the same TLS slot for sha1_of_bytes during their walks. */
extern void svnae_update_set_wc_root(const char *wc_root);
extern const char *svnae_update_get_wc_root(void);

static void
sha1_of_bytes(const char *data, int len, char out[65])
{
    out[0] = '\0';
    const char *root = svnae_update_get_wc_root();
    if (root) svnae_wc_hash_bytes(root, data, len, out);
}

struct rnode { char *path; int kind; char sha1[65]; char *data; int data_len; };
struct rtree { struct rnode *items; int n, cap; };

void *
svnae_mergert_new(void)
{
    return calloc(1, sizeof(struct rtree));
}

static void
rt_add(struct rtree *rt, const char *path, int kind, const char *data, int dlen)
{
    if (!rt) return;
    if (rt->n == rt->cap) {
        int nc = rt->cap ? rt->cap * 2 : 16;
        rt->items = realloc(rt->items, (size_t)nc * sizeof *rt->items);
        rt->cap = nc;
    }
    struct rnode *e = &rt->items[rt->n++];
    e->path = strdup(path ? path : "");
    e->kind = kind;
    e->sha1[0] = '\0';
    if (kind == 0 && data) {
        e->data = malloc((size_t)dlen + 1);
        memcpy(e->data, data, (size_t)dlen);
        e->data[dlen] = '\0';
        e->data_len = dlen;
        sha1_of_bytes(data, dlen, e->sha1);
    } else {
        e->data = NULL;
        e->data_len = 0;
    }
}

void svnae_mergert_add_dir(void *rt, const char *path) {
    rt_add((struct rtree *)rt, path, 1, NULL, 0);
}
void svnae_mergert_add_file(void *rt, const char *path,
                             const char *data, int data_len) {
    rt_add((struct rtree *)rt, path, 0, data, data_len);
}

void
svnae_mergert_free(void *rtv)
{
    struct rtree *rt = (struct rtree *)rtv;
    if (!rt) return;
    for (int i = 0; i < rt->n; i++) { free(rt->items[i].path); free(rt->items[i].data); }
    free(rt->items);
    free(rt);
}

int mergert_count(const struct rtree *rt) { return rt ? rt->n : 0; }
const char *mergert_path_at(const struct rtree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].path : "";
}
int mergert_kind_at(const struct rtree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].kind : -1;
}
const char *mergert_sha_at(const struct rtree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].sha1 : "";
}
const char *mergert_data_at(const struct rtree *rt, int i) {
    return (rt && i >= 0 && i < rt->n && rt->items[i].data) ? rt->items[i].data : "";
}
int mergert_data_len_at(const struct rtree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].data_len : 0;
}
int mergert_find_by_path(const struct rtree *rt, const char *path) {
    if (!rt || !path) return -1;
    for (int i = 0; i < rt->n; i++) {
        if (strcmp(rt->items[i].path, path) == 0) return i;
    }
    return -1;
}
