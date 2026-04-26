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

/* fs_fs/txn_shim.c — transaction accumulator.
 *
 * A txn holds a base revision and a list of pending edits. Edits are:
 *     ADD_FILE    path, content-bytes          — create a new file
 *     MOD_FILE    path, content-bytes          — replace an existing file
 *                                                 (same representation on-wire)
 *     MKDIR       path                         — create an empty directory
 *     DELETE      path                         — remove a file or directory
 *
 * Aether can't cheaply allocate byte blobs that travel through function
 * calls, so content for ADD_FILE/MOD_FILE is taken via `const char *` and
 * an explicit length (binary-safe).
 *
 * At commit time the Aether side walks the edit list bottom-up, applying
 * them against the base rev's tree. That walk is written in Aether; this
 * shim just provides storage for the edit list and a couple of read
 * accessors (so Aether can iterate without juggling arrays).
 *
 * We reuse the `struct svnae_buf` shape from the rest of the port so the
 * Aether `ptr` opaque handle means the same thing everywhere.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct svnae_buf {
    char *data;
    int   length;
};

static struct svnae_buf *buf_from(const unsigned char *src, int n) {
    struct svnae_buf *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->data = malloc((size_t)n + 1);
    if (!b->data) { free(b); return NULL; }
    if (n > 0) memcpy(b->data, src, n);
    b->data[n] = '\0';
    b->length = n;
    return b;
}

/* --- edit list --------------------------------------------------------- */

enum edit_kind {
    EDIT_ADD_FILE = 1,   /* ADD or replace; fs_fs storage is the same */
    EDIT_MKDIR    = 2,
    EDIT_DELETE   = 3,
    EDIT_COPY     = 4,   /* copy-from: target path points at existing sha1 */
};

struct edit {
    int   kind;
    char *path;      /* canonical: no leading '/', '/' separator, no trailing '/' */
    char *content;   /* only for ADD_FILE; malloc'd, binary-safe */
    int   content_len;
    /* For EDIT_COPY: */
    char *copy_from_sha1;  /* 40-char, already resolved by caller */
    int   copy_kind;       /* 0=file 1=dir */
};

struct svnae_txn {
    int   base_rev;
    struct edit *edits;
    int   n;
    int   cap;
};

struct svnae_txn *
svnae_txn_new(int base_rev)
{
    struct svnae_txn *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->base_rev = base_rev;
    return t;
}

int svnae_txn_base_rev(const struct svnae_txn *t) { return t ? t->base_rev : -1; }

static int
push_edit(struct svnae_txn *t)
{
    if (t->n == t->cap) {
        int ncap = t->cap ? t->cap * 2 : 8;
        struct edit *p = realloc(t->edits, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        t->edits = p;
        t->cap = ncap;
    }
    /* realloc() doesn't zero new storage; every caller expects the new
     * slot to be fully zeroed so free() of its (optional) fields at
     * teardown doesn't hit garbage. */
    memset(&t->edits[t->n], 0, sizeof t->edits[t->n]);
    return t->n++;
}

int
svnae_txn_add_file(struct svnae_txn *t, const char *path, const char *content, int len)
{
    if (!t) return -1;
    int idx = push_edit(t);
    if (idx < 0) return -1;
    struct edit *e = &t->edits[idx];
    e->kind = EDIT_ADD_FILE;
    e->path = strdup(path);
    if (len > 0 && content) {
        e->content = malloc((size_t)len);
        if (!e->content) { free(e->path); return -1; }
        memcpy(e->content, content, (size_t)len);
        e->content_len = len;
    } else {
        e->content = NULL;
        e->content_len = 0;
    }
    return 0;
}

int
svnae_txn_mkdir(struct svnae_txn *t, const char *path)
{
    if (!t) return -1;
    int idx = push_edit(t);
    if (idx < 0) return -1;
    struct edit *e = &t->edits[idx];
    e->kind = EDIT_MKDIR;
    e->path = strdup(path);
    e->content = NULL;
    e->content_len = 0;
    return 0;
}

int
svnae_txn_delete(struct svnae_txn *t, const char *path)
{
    if (!t) return -1;
    int idx = push_edit(t);
    if (idx < 0) return -1;
    struct edit *e = &t->edits[idx];
    e->kind = EDIT_DELETE;
    e->path = strdup(path);
    e->content = NULL;
    e->content_len = 0;
    return 0;
}

/* Copy-from: place `path` pointing at an existing (already-resolved)
 * sha1 with the given kind (0=file, 1=dir). The caller resolves
 * (from_path@base_rev) -> sha1 before calling this. */
int
svnae_txn_copy(struct svnae_txn *t, const char *path,
               const char *from_sha1, int from_kind)
{
    if (!t || !from_sha1 || strlen(from_sha1) != 40) return -1;
    int idx = push_edit(t);
    if (idx < 0) return -1;
    struct edit *e = &t->edits[idx];
    e->kind = EDIT_COPY;
    e->path = strdup(path);
    e->content = NULL;
    e->content_len = 0;
    e->copy_from_sha1 = strdup(from_sha1);
    e->copy_kind = from_kind;
    return 0;
}

int svnae_txn_count(const struct svnae_txn *t) { return t ? t->n : 0; }

int
svnae_txn_edit_kind(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return -1;
    return t->edits[i].kind;
}

const char *
svnae_txn_edit_path(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return "";
    return t->edits[i].path;
}

struct svnae_buf *
svnae_txn_edit_content(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return NULL;
    return buf_from((const unsigned char *)t->edits[i].content,
                    t->edits[i].content_len);
}

/* Raw-pointer accessors for the Aether rebuild_dir port. The Aether
 * side needs to pass content and copy_from_sha1 through to the rep
 * store writer without going through the opaque svnae_buf wrapper
 * (Aether strings + explicit lengths compose better). */
const char *
svnae_txn_edit_content_data(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return "";
    return t->edits[i].content ? t->edits[i].content : "";
}

int
svnae_txn_edit_content_len(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return 0;
    return t->edits[i].content_len;
}

const char *
svnae_txn_edit_copy_sha(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return "";
    return t->edits[i].copy_from_sha1 ? t->edits[i].copy_from_sha1 : "";
}

int
svnae_txn_edit_copy_kind(const struct svnae_txn *t, int i)
{
    if (!t || i < 0 || i >= t->n) return 0;
    return t->edits[i].copy_kind;
}

void
svnae_txn_free(struct svnae_txn *t)
{
    if (!t) return;
    for (int i = 0; i < t->n; i++) {
        free(t->edits[i].path);
        free(t->edits[i].content);
        free(t->edits[i].copy_from_sha1);
    }
    free(t->edits);
    free(t);
}

/* ---- a small dynamic string list ------------------------------------- *
 *
 * The commit algorithm gathers all paths affecting a given directory
 * (either directly or via a subpath) and groups them. A plain sorted-
 * unique list of strings is cheaper than our line-oriented sbuilders for
 * set operations.
 */

struct svnae_strset {
    char **items;
    int    n;
    int    cap;
};

struct svnae_strset *svnae_strset_new(void) { return calloc(1, sizeof(struct svnae_strset)); }

int
svnae_strset_add(struct svnae_strset *s, const char *v)
{
    if (!s) return -1;
    /* Skip duplicates. */
    for (int i = 0; i < s->n; i++)
        if (strcmp(s->items[i], v) == 0) return 0;
    if (s->n == s->cap) {
        int ncap = s->cap ? s->cap * 2 : 8;
        char **p = realloc(s->items, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        s->items = p;
        s->cap = ncap;
    }
    s->items[s->n++] = strdup(v);
    return 0;
}

int svnae_strset_count(const struct svnae_strset *s) { return s ? s->n : 0; }

const char *
svnae_strset_get(const struct svnae_strset *s, int i)
{
    if (!s || i < 0 || i >= s->n) return "";
    return s->items[i];
}

void
svnae_strset_free(struct svnae_strset *s)
{
    if (!s) return;
    for (int i = 0; i < s->n; i++) free(s->items[i]);
    free(s->items);
    free(s);
}

/* Recursive tree rebuilder lives in ae/fs_fs/rebuild.ae; rep-store
 * read/write stays in C via the externs. */

extern const char *aether_rebuild_dir(const char *repo,
                                      const char *base_dir_sha,
                                      const char *prefix,
                                      const struct svnae_txn *txn);

char *
svnae_txn_rebuild_root(const char *repo, const char *base_root_sha1,
                       const struct svnae_txn *txn)
{
    const char *sha = aether_rebuild_dir(repo,
                                         base_root_sha1 ? base_root_sha1 : "",
                                         "", txn);
    return (sha && *sha) ? strdup(sha) : NULL;
}

