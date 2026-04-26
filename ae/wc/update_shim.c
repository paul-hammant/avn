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

/* ae/wc/update_shim.c — remote_tree storage primitive for svn update.
 *
 * Round 69 (Gordian) ported svnae_wc_update + svnae_wc_switch
 * orchestration to ae/wc/update.ae. The recursive remote walk is
 * already in update_walk.ae and the two-pass apply is in
 * update_apply.ae; what remains here is just the C-allocated
 * remote_tree opaque struct + accessors that hold the per-node
 * binary content (since Aether strings can't safely carry blobs
 * with embedded NULs across FFI without going through length-aware
 * primitives that the apply pass already does on the C side).
 *
 *   svnae_rtree_new / _free                  — lifecycle
 *   svnae_rtree_add_dir / svnae_rtree_add_file — populate from RA walk
 *   rtree_count / _path_at / _kind_at / _sha_at / _data_at
 *   _data_len_at / _find_by_path             — accessors for the
 *                                                Aether apply pass
 *   update_sha1_of_file                      — TLS-buffered hex digest
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int svnae_wc_hash_bytes(const char *wc_root, const char *data, int len, char *out);
extern int svnae_wc_hash_file (const char *wc_root, const char *path, char *out);

/* The C side keeps wc_root in a TLS pointer so update_walk's
 * RA-cat-and-add-file path can compute the hex hash without threading
 * wc_root through every signature. The Aether-side update.ae sets it
 * via svnae_rtree_new wrapping; for now we accept "" and let the
 * apply pass compute hashes on demand. */
static __thread const char *g_wc_root = NULL;

void svnae_update_set_wc_root(const char *wc_root) { g_wc_root = wc_root; }

static void
sha1_of_bytes(const char *data, int len, char out[65])
{
    out[0] = '\0';
    if (g_wc_root) svnae_wc_hash_bytes(g_wc_root, data, len, out);
}

struct remote_node {
    char *path;
    int   kind;       /* 0=file 1=dir */
    char  sha1[65];
    char *data;       /* malloc'd, files only */
    int   data_len;
};

struct remote_tree {
    struct remote_node *items;
    int n, cap;
};

void *
svnae_rtree_new(void)
{
    return calloc(1, sizeof(struct remote_tree));
}

static void
rtree_add(struct remote_tree *rt, const char *path, int kind,
          const char *data, int data_len)
{
    if (!rt) return;
    if (rt->n == rt->cap) {
        int nc = rt->cap ? rt->cap * 2 : 16;
        rt->items = realloc(rt->items, (size_t)nc * sizeof *rt->items);
        rt->cap = nc;
    }
    struct remote_node *e = &rt->items[rt->n++];
    e->path = strdup(path ? path : "");
    e->kind = kind;
    e->sha1[0] = '\0';
    if (kind == 0 && data) {
        e->data = malloc((size_t)data_len + 1);
        memcpy(e->data, data, (size_t)data_len);
        e->data[data_len] = '\0';
        e->data_len = data_len;
        sha1_of_bytes(data, data_len, e->sha1);
    } else {
        e->data = NULL;
        e->data_len = 0;
    }
}

void svnae_rtree_add_dir(void *rt, const char *path) {
    rtree_add((struct remote_tree *)rt, path, 1, NULL, 0);
}

void svnae_rtree_add_file(void *rt, const char *path,
                          const char *data, int data_len) {
    rtree_add((struct remote_tree *)rt, path, 0, data, data_len);
}

void
svnae_rtree_free(void *rtv)
{
    struct remote_tree *rt = (struct remote_tree *)rtv;
    if (!rt) return;
    for (int i = 0; i < rt->n; i++) {
        free(rt->items[i].path);
        free(rt->items[i].data);
    }
    free(rt->items);
    free(rt);
}

int rtree_count(const struct remote_tree *rt) { return rt ? rt->n : 0; }
const char *rtree_path_at(const struct remote_tree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].path : "";
}
int rtree_kind_at(const struct remote_tree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].kind : -1;
}
const char *rtree_sha_at(const struct remote_tree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].sha1 : "";
}
const char *rtree_data_at(const struct remote_tree *rt, int i) {
    return (rt && i >= 0 && i < rt->n && rt->items[i].data) ? rt->items[i].data : "";
}
int rtree_data_len_at(const struct remote_tree *rt, int i) {
    return (rt && i >= 0 && i < rt->n) ? rt->items[i].data_len : 0;
}

int rtree_find_by_path(const struct remote_tree *rt, const char *path) {
    if (!rt || !path) return -1;
    for (int i = 0; i < rt->n; i++) {
        if (strcmp(rt->items[i].path, path) == 0) return i;
    }
    return -1;
}

/* Aether-callable disk-file hash. Returns a TLS scratch buffer with
 * the 65-byte hex digest (or "" on failure). The two-pass apply
 * compares this against the remote node's sha1 to detect dirty WCs. */
const char *
update_sha1_of_file(const char *wc_root, const char *path)
{
    static __thread char buf[65];
    buf[0] = '\0';
    if (svnae_wc_hash_file(wc_root, path, buf) != 0) buf[0] = '\0';
    return buf;
}
