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

/* ae/subr/packed_handle/shim.c — shared svnae_packed_handle storage,
 * used by ra/shim.c, repos/shim.c, and any other shim that holds an
 * Aether-parsed packed-record string with a per-handle pin_list. */

#include <stdlib.h>
#include <string.h>

#include "packed_handle.h"

struct svnae_packed_handle *
svnae_packed_handle_new(const char *packed, svnae_packed_count_fn count)
{
    if (!packed || !*packed) return NULL;
    struct svnae_packed_handle *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->packed = strdup(packed);
    if (!h->packed) { free(h); return NULL; }
    h->n = count ? count(h->packed) : 0;
    return h;
}

void
svnae_packed_handle_free(struct svnae_packed_handle *h)
{
    if (!h) return;
    free(h->packed);
    pin_list_free(&h->pins);
    free(h);
}
