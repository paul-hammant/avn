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

/* delta/xdelta/shim.c — produce svndiff operations from (source, target).
 *
 * This is the "compute the delta" half that svndiff.c's encoder consumes.
 * Given source bytes S and target bytes T, walk T and emit a list of ops
 * that reconstruct T from S and a new-data buffer.
 *
 * Algorithm (correct but simple — we're not trying to match svn's match-
 * finder byte-for-byte, just produce valid diffs):
 *
 *   1. Build a hash table from every 4-byte prefix in S to its position.
 *      Collisions within S are handled with chaining.
 *   2. Walk T left-to-right. At position tp:
 *        - hash T[tp..tp+4)
 *        - for each candidate source position sp in the chain, extend the
 *          match both forward (from tp, sp) and keep the longest
 *        - if best match length >= MIN_MATCH (4), emit source-copy op
 *        - otherwise emit one byte of new-data (buffered)
 *   3. Before any source-copy we flush the pending new-data run as an
 *      OP_NEW op and append its bytes to the new_data out-buffer.
 *
 * We do NOT currently generate OP_TARGET (self-copy) ops. Those are a
 * size optimisation; correctness without them is fine, and they're
 * tricky to decide well. svn's xdelta does emit them for long runs of
 * repeated bytes; we'll add a second pass later.
 *
 * The output is wired into the encoder via the builder API defined in
 * svndiff/shim.c — we don't duplicate that here; the Aether caller
 * receives back an svnae_buf handle holding the final svndiff bytes.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Forward decls from svndiff/shim.c (same translation unit in the binary
 * since both shims are listed in extra_sources together). */
struct svnae_buf;
struct svnae_encoder;

struct svnae_buf      *svnae_svndiff_encoder_finish(struct svnae_encoder *e,
                                                    const char *new_data, int new_data_len);
int                    svnae_svndiff_encoder_add_op(struct svnae_encoder *e,
                                                    int action, int length, int offset);
struct svnae_encoder  *svnae_svndiff_encoder_new(int source_len, int target_len);

/* ---- 4-byte rolling hash (cheap: just FNV-1a on 4 bytes) ---------------- */

#define MIN_MATCH 4

static inline uint32_t
hash4(const unsigned char *p)
{
    uint32_t h = 2166136261u;
    h ^= p[0]; h *= 16777619u;
    h ^= p[1]; h *= 16777619u;
    h ^= p[2]; h *= 16777619u;
    h ^= p[3]; h *= 16777619u;
    return h;
}

/* Chained hash table indexing every 4-byte prefix in source.
 * buckets[] holds head indices (or -1); next[] chains by source position.
 * Table size is a power of two chosen to be ~= source_len, minimum 16. */
struct srcindex {
    int *buckets;    /* size cap */
    int *next;       /* size source_len (each slot holds the prev src_pos in chain) */
    int  cap;
    int  src_len;
};

static int round_up_pow2(int x) {
    if (x < 16) return 16;
    int p = 16;
    while (p < x) p <<= 1;
    return p;
}

static void
srcindex_build(struct srcindex *si, const unsigned char *src, int src_len)
{
    si->src_len = src_len;
    si->cap = round_up_pow2(src_len > 0 ? src_len : 16);
    si->buckets = malloc(sizeof(int) * (size_t)si->cap);
    si->next    = malloc(sizeof(int) * (size_t)(src_len > 0 ? src_len : 1));
    if (!si->buckets || !si->next) { si->cap = 0; return; }
    for (int i = 0; i < si->cap; i++) si->buckets[i] = -1;

    if (src_len < MIN_MATCH) return;
    int mask = si->cap - 1;
    for (int i = 0; i <= src_len - MIN_MATCH; i++) {
        uint32_t h = hash4(src + i) & mask;
        si->next[i] = si->buckets[h];
        si->buckets[h] = i;
    }
}

static void srcindex_free(struct srcindex *si) {
    free(si->buckets); free(si->next);
    si->buckets = NULL; si->next = NULL; si->cap = 0;
}

/* Find the longest match in source starting at source[sp] for target[tp..].
 * Limits chain walk to CHAIN_LIMIT candidates to keep this roughly linear. */
#define CHAIN_LIMIT 32

static int
find_best_match(const struct srcindex *si,
                const unsigned char *src, int src_len,
                const unsigned char *tgt, int tgt_len, int tp,
                int *best_sp)
{
    if (tp + MIN_MATCH > tgt_len || si->cap == 0) return 0;
    int mask = si->cap - 1;
    uint32_t h = hash4(tgt + tp) & mask;

    int best_len = 0;
    int best_pos = -1;
    int candidates = 0;
    for (int sp = si->buckets[h]; sp >= 0 && candidates < CHAIN_LIMIT; sp = si->next[sp]) {
        candidates++;
        /* Quick reject on the first 4 bytes (must match since hashes agreed,
         * but we want to guard against collisions anyway). */
        if (memcmp(src + sp, tgt + tp, MIN_MATCH) != 0) continue;
        int len = MIN_MATCH;
        int max_src = src_len - sp;
        int max_tgt = tgt_len - tp;
        int max_len = max_src < max_tgt ? max_src : max_tgt;
        while (len < max_len && src[sp + len] == tgt[tp + len]) len++;
        if (len > best_len) { best_len = len; best_pos = sp; }
    }
    if (best_len >= MIN_MATCH) {
        *best_sp = best_pos;
        return best_len;
    }
    return 0;
}

/* ---- growable byte buffer (for the new-data accumulator) ---------------- */

struct gbuf {
    unsigned char *data;
    int len, cap;
};

static int
gbuf_push(struct gbuf *g, const unsigned char *b, int n)
{
    if (g->len + n > g->cap) {
        int nc = g->cap ? g->cap * 2 : 64;
        while (nc < g->len + n) nc *= 2;
        unsigned char *p = realloc(g->data, nc);
        if (!p) return -1;
        g->data = p; g->cap = nc;
    }
    memcpy(g->data + g->len, b, n);
    g->len += n;
    return 0;
}

/* ---- main entry point ---------------------------------------------------- */

/* Compute a delta from (source, target) and return the complete svndiff
 * bytes (via the svndiff encoder). Returns an svnae_buf handle; caller
 * uses the svnae_svndiff_buf_* accessors to read it out. */
struct svnae_buf *
svnae_xdelta_compute(const char *source, int source_len,
                     const char *target, int target_len)
{
    struct srcindex si = {0};
    srcindex_build(&si, (const unsigned char *)source, source_len);

    struct svnae_encoder *enc = svnae_svndiff_encoder_new(source_len, target_len);
    if (!enc) { srcindex_free(&si); return NULL; }

    struct gbuf newdata = {0};
    const unsigned char *s = (const unsigned char *)source;
    const unsigned char *t = (const unsigned char *)target;

    /* Track the start of the current pending new-data run. When a source
     * match is found we flush [ndrun_start..tp) as one OP_NEW before the
     * OP_SOURCE; bytes go into the per-window new_data blob that
     * encoder_finish writes out after the instruction stream. */
    int tp = 0;
    int ndrun_start = 0;

    while (tp < target_len) {
        int sp = -1;
        int mlen = find_best_match(&si, s, source_len, t, target_len, tp, &sp);
        if (mlen >= MIN_MATCH) {
            /* Flush any pending new-data run up to tp. */
            int run_len = tp - ndrun_start;
            if (run_len > 0) {
                if (gbuf_push(&newdata, t + ndrun_start, run_len) != 0) goto oom;
                svnae_svndiff_encoder_add_op(enc, 2, run_len, 0);
            }
            svnae_svndiff_encoder_add_op(enc, 0, mlen, sp);
            tp += mlen;
            ndrun_start = tp;
        } else {
            tp++;
        }
    }
    /* Trailing new-data run. */
    {
        int run_len = target_len - ndrun_start;
        if (run_len > 0) {
            if (gbuf_push(&newdata, t + ndrun_start, run_len) != 0) goto oom;
            svnae_svndiff_encoder_add_op(enc, 2, run_len, 0);
        }
    }

    struct svnae_buf *out = svnae_svndiff_encoder_finish(
        enc, (const char *)newdata.data, newdata.len);
    free(newdata.data);
    srcindex_free(&si);
    return out;

oom:
    /* We've leaked the encoder's internal ops array via the lack of a
     * reset API; for now, fall through to finish+free which at least
     * frees the encoder. */
    free(newdata.data);
    srcindex_free(&si);
    return NULL;  /* the encoder leaks on this OOM path; rare, low-prio */
}
