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

/* ae/subr/packed_handle/packed_handle.h — shared opaque handle for
 * packed-string accessor families.
 *
 * Both ae/ra/shim.c and ae/repos/shim.c expose multiple accessor
 * groups (log / list / paths / blame / info) that all share the
 * same shape: own one packed-string payload (parsed Aether-side),
 * remember the row count, and pin per-accessor strdup'd copies so
 * callers can hold pointers across calls.
 *
 * Round 45 collapsed four ra/ families onto a private shared
 * struct svnae_ra_handle. Round 53 did the same for repos/ (with
 * its own private svnae_repos_handle). Round 56 extracts the one
 * shared definition into this header so both shims point at the
 * same struct and the same {new, free, pin} helpers.
 *
 * Per-domain wrappers in each shim are still distinct C functions
 * (svnae_ra_log_author vs svnae_repos_log_author) — the public ABI
 * stays exactly as it was. Only the storage primitive is unified.
 */

#ifndef AE_SUBR_PACKED_HANDLE_H
#define AE_SUBR_PACKED_HANDLE_H

#include "../pin_list.h"

struct svnae_packed_handle {
    char *packed;
    int   n;
    struct pin_list pins;
};

typedef int (*svnae_packed_count_fn)(const char *packed);

/* Allocate a handle owning a fresh strdup of `packed`. `count` is
 * nullable — pass NULL for single-record handles (the info family).
 * Returns NULL when `packed` is empty/missing or on OOM. */
struct svnae_packed_handle *
svnae_packed_handle_new(const char *packed, svnae_packed_count_fn count);

/* Free the handle plus every pinned string in its pin_list. */
void svnae_packed_handle_free(struct svnae_packed_handle *h);

#endif /* AE_SUBR_PACKED_HANDLE_H */
