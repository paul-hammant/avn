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

/* ---- accessor helpers --------------------------------------------- *
 *
 * Every per-domain accessor in ra/shim.c and repos/shim.c follows
 * one of three shapes — count, indexed int, indexed pinned-string —
 * differing only in the underlying packed-string accessor function.
 * Round 60 collapsed the cast + bounds-check + dispatch boilerplate
 * into these inline helpers so each per-domain accessor becomes a
 * one-line forward.
 *
 * The `(void *)` cast at every call site is deliberate: the per-
 * domain wrappers take their own opaque type (struct svnae_ra_log *
 * etc.) and we want a single helper that accepts any of them. C's
 * void* implicitly converts both ways without a warning. */

typedef int         (*svnae_packed_int_fn)(const char *packed, int i);
typedef const char *(*svnae_packed_str_fn)(const char *packed, int i);

/* Row count. NULL handle returns 0 (matches every caller's contract). */
static inline int
svnae_packed_count(const void *handle)
{
    const struct svnae_packed_handle *h = (const struct svnae_packed_handle *)handle;
    return h ? h->n : 0;
}

/* Indexed int accessor. Out-of-range or NULL handle returns -1, which
 * is the sentinel every existing caller checks for. */
static inline int
svnae_packed_int_at(const void *handle, int i, svnae_packed_int_fn fn)
{
    const struct svnae_packed_handle *h = (const struct svnae_packed_handle *)handle;
    if (!h || i < 0 || i >= h->n) return -1;
    return fn(h->packed, i);
}

/* Indexed pinned-string accessor. Out-of-range or NULL handle returns
 * "". The returned pointer is owned by the handle's pin_list and is
 * stable across other accessor calls until the handle is freed. */
static inline const char *
svnae_packed_pin_at(void *handle, int i, svnae_packed_str_fn fn)
{
    struct svnae_packed_handle *h = (struct svnae_packed_handle *)handle;
    if (!h || i < 0 || i >= h->n) return "";
    return pin_str(&h->pins, fn(h->packed, i));
}

/* Single-record (no-index) pinned-string accessor for the info-style
 * handle. Returns "" on NULL handle. The function is called against
 * h->packed alone — single-record families (svnae_ra_info,
 * svnae_repos_info) park their fields under the field name, no
 * index. */
typedef const char *(*svnae_packed_str_fn0)(const char *packed);

static inline const char *
svnae_packed_pin_field(void *handle, svnae_packed_str_fn0 fn)
{
    struct svnae_packed_handle *h = (struct svnae_packed_handle *)handle;
    if (!h) return "";
    return pin_str(&h->pins, fn(h->packed));
}

/* Indexed int accessor with a custom out-of-range sentinel — used by
 * svnae_ra_info_rev_num and friends that return -1 on miss without
 * the bounds check (single-record handle). */
typedef int (*svnae_packed_int_fn0)(const char *packed);

static inline int
svnae_packed_int_field(const void *handle, svnae_packed_int_fn0 fn)
{
    const struct svnae_packed_handle *h = (const struct svnae_packed_handle *)handle;
    return h ? fn(h->packed) : -1;
}

#endif /* AE_SUBR_PACKED_HANDLE_H */
