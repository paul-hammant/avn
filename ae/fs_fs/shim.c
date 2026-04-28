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

/* fs_fs/shim.c — small storage primitives the rest of fs_fs and the
 * test suites reach via FFI:
 *   svnae_buf:           binary-safe (data, length) holder + accessors
 *   svnae_fsfs_read_small_file:  binary slurp returning svnae_buf
 *   svnae_fsfs_now_iso8601:      TLS-cached UTC timestamp string
 *   svnae_tree_builder:  (path, kind, content) tuple list for tests
 *   svnae_sbuilder:      growable string buffer
 * Everything else in fs_fs is in *.ae now. */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* svnae_fsfs_read_small_file + the svnae_buf wrapper trio +
 * buf_from helper retired in Rounds 137–138. .ae callers now use
 * aether_io_read_file (text path) and svnae_fsfs_tree_builder_
 * content_data / _len (binary path) directly. */

/* Current ISO-8601 UTC timestamp, without milliseconds. For the revision
 * blob's `date:` field. Returned string is static and must be used before
 * the next call. */
extern char *os_now_utc_iso8601_raw(void);

const char *
svnae_fsfs_now_iso8601(void)
{
    /* The caller-lifetime contract here is "valid until the next
     * call"; std.os's raw extern hands back a malloc we'd have to
     * free, so cache into a TLS buffer for drop-in behaviour. */
    static __thread char buf[32];
    char *s = os_now_utc_iso8601_raw();
    if (!s) { buf[0] = '\0'; return buf; }
    size_t n = strlen(s);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, s, n);
    buf[n] = '\0';
    free(s);
    return buf;
}

/* ---- tree builder ----------------------------------------------------- *
 *
 * (path, kind, content) tuple list. kind 0 = file (content kept), 1 =
 * dir (content ignored). Strings are strdup'd at add-time so callers
 * can free their buffers immediately. Used by the test suite — Aether
 * walks the builder to group by parent and emit dir blobs bottom-up. */

struct tree_entry {
    char *path;      /* "a/b/c.txt" — no leading slash, '/' separator */
    int   kind;      /* 0 = file, 1 = dir */
    char *content;   /* malloc'd, length = content_len. NULL for dirs. */
    int   content_len;
};

struct svnae_tree_builder {
    struct tree_entry *entries;
    int                n;
    int                cap;
};

struct svnae_tree_builder *
svnae_fsfs_tree_builder_new(void)
{
    return calloc(1, sizeof(struct svnae_tree_builder));
}

int
svnae_fsfs_tree_builder_add(struct svnae_tree_builder *tb,
                            const char *path, int kind,
                            const char *content, int content_len)
{
    if (!tb) return -1;
    if (tb->n == tb->cap) {
        int ncap = tb->cap ? tb->cap * 2 : 8;
        struct tree_entry *p = realloc(tb->entries, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        tb->entries = p;
        tb->cap = ncap;
    }
    struct tree_entry *e = &tb->entries[tb->n];
    e->path = strdup(path);
    e->kind = kind;
    if (kind == 0 && content_len > 0 && content) {
        e->content = malloc((size_t)content_len);
        if (!e->content) { free(e->path); return -1; }
        memcpy(e->content, content, (size_t)content_len);
        e->content_len = content_len;
    } else {
        e->content = NULL;
        e->content_len = 0;
    }
    tb->n++;
    return 0;
}

int
svnae_fsfs_tree_builder_count(const struct svnae_tree_builder *tb)
{
    return tb ? tb->n : 0;
}

const char *
svnae_fsfs_tree_builder_path(const struct svnae_tree_builder *tb, int i)
{
    if (!tb || i < 0 || i >= tb->n) return "";
    return tb->entries[i].path;
}

int
svnae_fsfs_tree_builder_kind(const struct svnae_tree_builder *tb, int i)
{
    if (!tb || i < 0 || i >= tb->n) return -1;
    return tb->entries[i].kind;
}

/* Raw content + length accessors. Mirrors txn_shim's content_data /
 * content_len pair — explicit length composes better with .ae
 * callers' string.length / std.bytes idioms than the svnae_buf
 * handle did. The svnae_buf-returning shape was retired in Round
 * 138 along with its svnae_fsfs_buf_* accessor trio. */
const char *
svnae_fsfs_tree_builder_content_data(const struct svnae_tree_builder *tb, int i)
{
    if (!tb || i < 0 || i >= tb->n) return "";
    return tb->entries[i].content ? tb->entries[i].content : "";
}

int
svnae_fsfs_tree_builder_content_len(const struct svnae_tree_builder *tb, int i)
{
    if (!tb || i < 0 || i >= tb->n) return 0;
    return tb->entries[i].content_len;
}

void
svnae_fsfs_tree_builder_free(struct svnae_tree_builder *tb)
{
    if (!tb) return;
    for (int i = 0; i < tb->n; i++) {
        free(tb->entries[i].path);
        free(tb->entries[i].content);
    }
    free(tb->entries);
    free(tb);
}

/* svnae_fsfs_sb_* string-builder retired in Round 110 — the only
 * caller (test_revisions.ae) switched to plain string.concat now
 * that std.bytes (issue #288) gives us a real mutable buffer for
 * the binary-codec workloads that actually need one. The sbuilder
 * was a 50-line growable-buffer optimisation for an O(n²) test
 * fixture, where n is small. */
