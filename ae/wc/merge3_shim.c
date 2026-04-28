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

typedef struct { const char *_0; int _1; } _tuple_string_int;

extern _tuple_string_int aether_merge3(const void *mine, int mine_len,
                                        const void *base, int base_len,
                                        const void *theirs, int theirs_len);

extern int aether_merge3_apply(const char *wc_path,
                                const void *base, int base_len, int base_rev,
                                const void *theirs, int theirs_len, int theirs_rev);

/* Public buffer-mode merge3.
 *   mine/base/theirs — three byte buffers
 *   out_merged       — receives malloc'd merge result
 *   out_len          — its length
 * Returns 0 (clean), 1 (conflicts), -1 (error). */
int
svnae_merge3(const char *mine,   int mine_len,
             const char *base,   int base_len,
             const char *theirs, int theirs_len,
             char **out_merged,  int *out_len)
{
    if (mine_len < 0 || base_len < 0 || theirs_len < 0) return -1;

    AetherString *mine_s   = string_new_with_length(mine   ? mine   : "", (size_t)mine_len);
    AetherString *base_s   = string_new_with_length(base   ? base   : "", (size_t)base_len);
    AetherString *theirs_s = string_new_with_length(theirs ? theirs : "", (size_t)theirs_len);

    _tuple_string_int r = aether_merge3(mine_s, mine_len,
                                         base_s, base_len,
                                         theirs_s, theirs_len);

    string_free(mine_s); string_free(base_s); string_free(theirs_s);

    /* status: -1 = error, 0 = clean, 1 = conflicts. */
    if (r._1 < 0) { *out_merged = NULL; *out_len = 0; return -1; }
    if (r._1 > 1) { *out_merged = NULL; *out_len = 0; return -1; }
    int n = (int)aether_string_length(r._0);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { *out_merged = NULL; *out_len = 0; return -1; }
    if (n > 0) memcpy(buf, aether_string_data(r._0), (size_t)n);
    buf[n] = '\0';
    *out_merged = buf;
    *out_len    = n;
    return r._1;
}

void svnae_merge3_free(char *p) { free(p); }

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
