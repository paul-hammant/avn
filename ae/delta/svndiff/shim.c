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

/* delta/svndiff/shim.c — encode and decode svndiff streams.
 *
 * We implement svndiff1 with per-window compression disabled (the "always
 * raw" sentinel both sections use, so the instructions and new-data are
 * written verbatim). This keeps the code small and debuggable. svn's own
 * reader accepts this as valid v1 input because the sentinel is part of
 * the spec. Window-level zlib compression can be layered on later — it's
 * an output-size optimisation, not a correctness requirement.
 *
 * The encoder operates on a single window at a time. A txdelta stream is
 * a sequence of windows; callers that want to stream incrementally will
 * write one at a time. For Phase 2 we ship the one-shot "whole-buffer
 * source → whole-buffer target via one window" form, which is enough for
 * the round-trip test and for most small files.
 *
 * Aether binds these through the usual svnae_buf handle (see compress/shim)
 * so that byte streams with embedded NULs round-trip correctly.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Reuse the compress shim's buf API — same shape. We duplicate the small
 * struct here because Aether has no equivalent of C headers shared across
 * shims; at the Aether level this is still just an opaque `ptr`. */
struct svnae_buf {
    char *data;
    int   length;
};

static struct svnae_buf *buf_from(const unsigned char *src, int n) {
    struct svnae_buf *b = malloc(sizeof *b);
    if (!b) return NULL;
    /* Always allocate n+1 and NUL-terminate so buf_data returns a string
     * that's safe to print even when the payload itself has no trailing
     * NUL. Embedded NULs still propagate via buf_length. */
    b->data = malloc((size_t)n + 1);
    if (!b->data) { free(b); return NULL; }
    if (n > 0) memcpy(b->data, src, n);
    b->data[n] = '\0';
    b->length = n;
    return b;
}

/* ---- varint (base-128, MSB-continuation, big-endian) ------------------- */

static int
vi_encode(uint64_t v, unsigned char *out)
{
    /* Count bytes needed. */
    int n = 1;
    uint64_t t = v >> 7;
    while (t > 0) { n++; t >>= 7; }
    int written = n;
    /* Write MSB-first with continuation bits on all but the last byte. */
    int i = n - 1;
    out[i] = (unsigned char)(v & 0x7f);
    while (--i >= 0) {
        v >>= 7;
        out[i] = (unsigned char)((v & 0x7f) | 0x80);
    }
    (void)written;  /* silence unused in some builds */
    return n;
}

/* Decode varint at p (bounded by end). Returns # bytes consumed, or 0 on
 * malformed/overflow. Writes the value to *val. */
static int
vi_decode(const unsigned char *p, const unsigned char *end, uint64_t *val)
{
    uint64_t v = 0;
    const unsigned char *start = p;
    while (p < end) {
        unsigned char c = *p++;
        if ((c & 0x80) == 0) {
            v = (v << 7) | c;
            *val = v;
            return (int)(p - start);
        }
        v = (v << 7) | (c & 0x7f);
        if (p - start > 10) return 0;  /* guard against runaway */
    }
    return 0;
}

/* ---- instruction encoding --------------------------------------------- */

/* Action codes: 0=source, 1=target, 2=new. Emit one instruction into `out`.
 * `has_offset` is 1 for source/target, 0 for new. Returns # bytes written. */
static int
inst_encode(int action, uint32_t length, uint64_t offset, int has_offset,
            unsigned char *out)
{
    int n = 0;
    /* Pack length into the bottom 6 bits if it fits, else 0 + varint. */
    if (length < 0x40) {
        out[n++] = (unsigned char)((action << 6) | length);
    } else {
        out[n++] = (unsigned char)(action << 6);
        n += vi_encode(length, out + n);
    }
    if (has_offset) {
        n += vi_encode(offset, out + n);
    }
    return n;
}

/* ---- write helpers that grow a heap buffer ---------------------------- */

struct growable {
    unsigned char *data;
    int len;
    int cap;
};

static int
g_reserve(struct growable *g, int extra)
{
    int need = g->len + extra;
    if (need <= g->cap) return 0;
    int ncap = g->cap == 0 ? 64 : g->cap;
    while (ncap < need) ncap *= 2;
    unsigned char *p = realloc(g->data, ncap);
    if (!p) return -1;
    g->data = p;
    g->cap = ncap;
    return 0;
}

static int
g_push(struct growable *g, const unsigned char *b, int n)
{
    if (g_reserve(g, n) != 0) return -1;
    memcpy(g->data + g->len, b, n);
    g->len += n;
    return 0;
}

static int
g_push_varint(struct growable *g, uint64_t v)
{
    unsigned char buf[10];
    int n = vi_encode(v, buf);
    return g_push(g, buf, n);
}

/* ---- encode a single-window svndiff ----------------------------------- *
 *
 * The caller supplies the instruction list already computed (by xdelta.c
 * or, for a dumb encoder, by walking source+target). Each op is a tuple
 * (action, length, offset). `new_data` is the bytes referenced by the
 * `new` op type; if there are no `new` ops, pass NULL/0.
 *
 * We accept the op list as a flat int array: [action, length, offset,
 * action, length, offset, ...]. Action code must be 0, 1, or 2; for
 * action 2 (new), the offset is ignored on the wire but still needs to
 * be passed as 0 so the caller's flat array format is regular.
 *
 * Returns an svnae_buf handle owning the output, or NULL on failure.
 */
struct svnae_buf *
svnae_svndiff_encode_window(
    int source_len,                 /* uncompressed source window size */
    int target_len,                 /* uncompressed target size */
    const int *ops, int nops,       /* ops as [action,length,offset]*nops */
    const char *new_data, int new_data_len)
{
    /* Encode the instruction section into a temporary. */
    struct growable inst = {0};
    for (int i = 0; i < nops; i++) {
        int action = ops[i * 3];
        uint32_t length = (uint32_t)ops[i * 3 + 1];
        uint64_t offset = (uint64_t)(uint32_t)ops[i * 3 + 2];
        if (action < 0 || action > 2) { free(inst.data); return NULL; }

        unsigned char ibuf[32];
        int ilen = inst_encode(action, length, offset, action != 2, ibuf);
        if (g_push(&inst, ibuf, ilen) != 0) { free(inst.data); return NULL; }
    }

    /* Assemble the final output. Header then window. */
    struct growable out = {0};
    /* svndiff1 signature */
    unsigned char sig[4] = { 'S', 'V', 'N', 1 };
    if (g_push(&out, sig, 4) != 0) goto oom;

    /* Window header: sview_offset, sview_len, tview_len, inst_len, newdata_len */
    if (g_push_varint(&out, 0) != 0) goto oom;                   /* sview_offset */
    if (g_push_varint(&out, (uint64_t)source_len) != 0) goto oom;
    if (g_push_varint(&out, (uint64_t)target_len) != 0) goto oom;
    if (g_push_varint(&out, (uint64_t)inst.len) != 0) goto oom;
    if (g_push_varint(&out, (uint64_t)new_data_len) != 0) goto oom;

    /* Instruction payload (uncompressed). */
    if (g_push(&out, inst.data, inst.len) != 0) goto oom;
    /* New-data payload (uncompressed). */
    if (new_data_len > 0 && new_data) {
        if (g_push(&out, (const unsigned char *)new_data, new_data_len) != 0) goto oom;
    }

    free(inst.data);
    struct svnae_buf *b = buf_from(out.data, out.len);
    free(out.data);
    return b;

oom:
    free(inst.data);
    free(out.data);
    return NULL;
}

/* ---- decode + apply -------------------------------------------------- *
 *
 * Read a single-window svndiff stream and reconstruct the target, given
 * the source. Returns an svnae_buf with the target bytes, or NULL on
 * malformed input / memory failure.
 *
 * svndiff supports multi-window streams; we handle one window for now.
 * A multi-window reader is a straightforward extension and will land
 * when fs_fs starts emitting windowed diffs for large files.
 */
struct svnae_buf *
svnae_svndiff_decode_apply(
    const char *diff, int diff_len,
    const char *source, int source_len)
{
    const unsigned char *p   = (const unsigned char *)diff;
    const unsigned char *end = p + diff_len;

    /* Signature */
    if (diff_len < 4) return NULL;
    if (p[0] != 'S' || p[1] != 'V' || p[2] != 'N' || p[3] != 1) return NULL;
    p += 4;

    uint64_t sview_offset = 0, sview_len = 0, tview_len = 0;
    uint64_t inst_len = 0, newdata_len = 0;
    int n;
    n = vi_decode(p, end, &sview_offset); if (n == 0) return NULL; p += n;
    n = vi_decode(p, end, &sview_len);    if (n == 0) return NULL; p += n;
    n = vi_decode(p, end, &tview_len);    if (n == 0) return NULL; p += n;
    n = vi_decode(p, end, &inst_len);     if (n == 0) return NULL; p += n;
    n = vi_decode(p, end, &newdata_len);  if (n == 0) return NULL; p += n;

    if (sview_offset + sview_len > (uint64_t)source_len) return NULL;
    if ((uint64_t)(end - p) < inst_len + newdata_len)   return NULL;

    const unsigned char *inst_p    = p;
    const unsigned char *inst_end  = p + inst_len;
    const unsigned char *newdata_p = p + inst_len;

    /* Allocate target buffer. */
    unsigned char *target = malloc(tview_len > 0 ? tview_len + 1 : 1);
    if (!target) return NULL;
    uint64_t tpos = 0;
    uint64_t new_consumed = 0;

    while (inst_p < inst_end) {
        unsigned char c = *inst_p++;
        int action = (c >> 6) & 0x3;
        if (action > 2) { free(target); return NULL; }

        uint64_t length = c & 0x3f;
        if (length == 0) {
            uint64_t v;
            n = vi_decode(inst_p, inst_end, &v);
            if (n == 0) { free(target); return NULL; }
            inst_p += n;
            length = v;
        }

        uint64_t offset = 0;
        if (action != 2) {
            n = vi_decode(inst_p, inst_end, &offset);
            if (n == 0) { free(target); return NULL; }
            inst_p += n;
        }

        if (tpos + length > tview_len) { free(target); return NULL; }

        if (action == 0) {
            /* source copy */
            if (offset + length > sview_len) { free(target); return NULL; }
            memcpy(target + tpos,
                   (const unsigned char *)source + sview_offset + offset,
                   length);
            tpos += length;
        } else if (action == 1) {
            /* target self-copy — byte-at-a-time because of the overlap trick */
            if (offset >= tpos) { free(target); return NULL; }
            for (uint64_t i = 0; i < length; i++) {
                target[tpos + i] = target[offset + i];
            }
            tpos += length;
        } else {
            /* new-data copy */
            if (new_consumed + length > newdata_len) { free(target); return NULL; }
            memcpy(target + tpos, newdata_p + new_consumed, length);
            new_consumed += length;
            tpos += length;
        }
    }

    if (tpos != tview_len) { free(target); return NULL; }

    target[tview_len] = '\0';
    struct svnae_buf *b = buf_from(target, (int)tview_len);
    free(target);
    return b;
}

/* ---- builder API for Aether callers ---------------------------------- *
 *
 * Aether can't cheaply allocate a flat C int array, so we expose a builder:
 *     h = encoder_new(source_len, target_len)
 *     encoder_add_op(h, action, length, offset)
 *     ...
 *     diff = encoder_finish(h, new_data, new_data_len)  // frees h implicitly
 *     or   encoder_free(h) if aborting
 *
 * Internally it just accumulates ops into a growable int array and then
 * hands off to svnae_svndiff_encode_window at finish time.
 */
struct svnae_encoder {
    int source_len;
    int target_len;
    int *ops;
    int nops;
    int cap;
};

struct svnae_encoder *
svnae_svndiff_encoder_new(int source_len, int target_len)
{
    struct svnae_encoder *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->source_len = source_len;
    e->target_len = target_len;
    return e;
}

int
svnae_svndiff_encoder_add_op(struct svnae_encoder *e, int action, int length, int offset)
{
    if (!e) return -1;
    if ((e->nops + 1) * 3 > e->cap) {
        int ncap = e->cap == 0 ? 16 : e->cap * 2;
        int *p = realloc(e->ops, (size_t)ncap * sizeof(int));
        if (!p) return -1;
        e->ops = p;
        e->cap = ncap;
    }
    e->ops[e->nops * 3    ] = action;
    e->ops[e->nops * 3 + 1] = length;
    e->ops[e->nops * 3 + 2] = offset;
    e->nops++;
    return 0;
}

struct svnae_buf *
svnae_svndiff_encoder_finish(struct svnae_encoder *e,
                             const char *new_data, int new_data_len)
{
    if (!e) return NULL;
    struct svnae_buf *out = svnae_svndiff_encode_window(
        e->source_len, e->target_len, e->ops, e->nops,
        new_data, new_data_len);
    free(e->ops);
    free(e);
    return out;
}

void svnae_svndiff_encoder_free(struct svnae_encoder *e)
{
    if (!e) return;
    free(e->ops);
    free(e);
}

/* Accessor/free reuse the same buf shape. We redeclare the symbols here
 * rather than depending on compress/shim.c — each binary pulls only the
 * shim it cites in extra_sources, and we don't want svndiff to pull in
 * compress unless the caller asks. */
int         svnae_svndiff_buf_length(const struct svnae_buf *b) { return b ? b->length : 0; }
const char *svnae_svndiff_buf_data  (const struct svnae_buf *b) { return b ? b->data : ""; }
void        svnae_svndiff_buf_free  (struct svnae_buf *b)
{
    if (!b) return;
    free(b->data);
    free(b);
}
