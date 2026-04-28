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

/* ae/wc/merge3_shim.c — thin C-side ABI wrapper around the Aether
 * three-way merge in ae/wc/merge3.ae.
 *
 * Round 112 hoisted the body — fork/pipe/exec/wait dance for
 * /usr/bin/diff3 plus tmp-file management plus sidecar drops —
 * into Aether using os.run_capture (which started returning
 * (stdout, status, err) in aether 0.94, issue #289). Round 121
 * (aether 0.99 #285) switched aether_merge3's return from a
 * leading-status-byte channel to a clean (string, int) tuple,
 * which collapses the byte-strip dance below to a tuple
 * destructure plus a malloc-and-copy.
 */

#include <stdlib.h>
#include <string.h>
#include "aether_string.h"

extern int aether_merge3_apply(const char *wc_path,
                                const void *base, int base_len, int base_rev,
                                const void *theirs, int theirs_len, int theirs_rev);

/* svnae_merge3 (buffer-returning variant) + svnae_merge3_free
 * retired in Round 144 — no callers. The on-disk variant
 * svnae_merge3_apply (below) is what every actual user (update_
 * apply.ae, merge_apply.ae) calls. */

/* Apply 3-way merge to an on-disk WC file. See merge3.ae's
 * aether_merge3_apply for the semantics. Returns 0/1/-1. */
int
svnae_merge3_apply(const char *wc_path,
                   const char *base, int base_len, int base_rev,
                   const char *theirs, int theirs_len, int theirs_rev)
{
    if (base_len < 0 || theirs_len < 0) return -1;
    AetherString *base_s   = string_new_with_length(base   ? base   : "", (size_t)base_len);
    AetherString *theirs_s = string_new_with_length(theirs ? theirs : "", (size_t)theirs_len);
    int rc = aether_merge3_apply(wc_path, base_s, base_len, base_rev,
                                  theirs_s, theirs_len, theirs_rev);
    string_free(base_s); string_free(theirs_s);
    return rc;
}
