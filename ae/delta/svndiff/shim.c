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

/* delta/svndiff/shim.c — svndiff1 encode/decode (per-window
 * compression disabled — instructions and new-data written verbatim;
 * the "always raw" sentinel is part of v1 so svn's reader accepts
 * this). One-shot "whole-buffer source → one window" form.
 * Aether binds via svnae_buf handles for binary-safe round trip. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "aether_string.h"   /* aether_string_data / aether_string_length */

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
/* Decoder body lives in ae/delta/svndiff_decode.ae now (Round 108
 * — std.bytes from aether 0.94 unblocked the action-1 RLE-overlap
 * pattern that needed mutable random-access byte writes). The C
 * side does two boundary jobs:
 *   (1) Wrap the raw `diff` and `source` byte buffers into
 *       length-aware AetherStrings before crossing into Aether.
 *       Aether's `string.char_at` and `string.length` use strlen()
 *       on raw `char*` pointers, which truncates at the first
 *       embedded NUL. Wrapping via string_new_with_length carries
 *       the explicit length end-to-end.
 *   (2) Repack the Aether result into the legacy svnae_buf handle
 *       so existing callers don't change.
 */
/* Result format: empty AetherString = error; otherwise byte 0 is
 * the status flag (0x01 = success) and bytes 1.. are the payload.
 * See ae/delta/svndiff_decode.ae for why this leading-byte channel
 * exists rather than a tuple return. */
extern const char *aether_svndiff_decode_apply(const void *diff, int diff_len,
                                                const void *source, int source_len);

struct svnae_buf *
svnae_svndiff_decode_apply(const char *diff, int diff_len,
                           const char *source, int source_len)
{
    if (diff_len < 0 || source_len < 0) return NULL;
    AetherString *diff_s = string_new_with_length(diff ? diff : "", (size_t)diff_len);
    AetherString *src_s  = string_new_with_length(source ? source : "", (size_t)source_len);
    const char *out = aether_svndiff_decode_apply(diff_s, diff_len, src_s, source_len);
    string_free(diff_s);
    string_free(src_s);
    if (!out) return NULL;
    int n = (int)aether_string_length(out);
    if (n == 0) return NULL;                                /* error */
    const char *data = aether_string_data(out);
    if (data[0] != 0x01) return NULL;                       /* unknown status */
    /* Strip the status byte. n - 1 may be 0 (legitimate empty target). */
    return buf_from((const unsigned char *)(data + 1), n - 1);
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
