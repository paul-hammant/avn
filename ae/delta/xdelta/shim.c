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

/* delta/xdelta/shim.c — thin AetherString-wrap adapter for the
 * Aether-side xdelta implementation.
 *
 * Round 122 (aether 0.99 + std.bytes from 0.94) ported the
 * algorithm body — match-finder + new-data accumulator — into
 * ae/delta/xdelta.ae. The chained 4-byte hash table dropped to
 * a plain linear scan: O(N²) instead of O(N) per lookup, but
 * the test workloads are kiB-scale and the simpler shape is
 * easier to read in Aether (no aether_int_array equivalent
 * yet). Real-repo-scale workloads would want the chained hash
 * back; revisit when the delta hits a real-size workload.
 *
 * The svndiff encoder handle (svnae_svndiff_encoder_new /
 * _add_op / _finish / _free) and the resulting svnae_buf are
 * still C-side — Aether uses them via the existing externs.
 */

#include "aether_string.h"   /* aether_string_data / aether_string_length */

struct svnae_buf;

extern void *aether_xdelta_compute(const void *source, int source_len,
                                    const void *target, int target_len);

struct svnae_buf *
svnae_xdelta_compute(const char *source, int source_len,
                     const char *target, int target_len)
{
    if (source_len < 0 || target_len < 0) return NULL;

    /* Wrap into AetherStrings so length crosses the FFI boundary
     * intact; aether_string_length on a raw char* would strlen-
     * truncate at the first NUL in binary content. */
    AetherString *src_s = string_new_with_length(source ? source : "", (size_t)source_len);
    AetherString *tgt_s = string_new_with_length(target ? target : "", (size_t)target_len);

    void *out = aether_xdelta_compute(src_s, source_len, tgt_s, target_len);

    string_free(src_s);
    string_free(tgt_s);
    return (struct svnae_buf *)out;
}
