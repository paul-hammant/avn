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
 * (stdout, status, err) in aether 0.94, issue #289). The C side
 * keeps two job:
 *   - svnae_merge3: receives raw (mine, base, theirs) byte
 *     buffers + the (out_merged, out_len) malloc-detach contract
 *     the C-side callers historically used. Wraps the byte buffers
 *     into AetherStrings, calls aether_merge3, parses the leading-
 *     status-byte channel, hands back malloc'd bytes.
 *   - svnae_merge3_apply: forwards to aether_merge3_apply, which
 *     does the wc_path read + write_atomic + sidecar drops itself.
 */

#include <stdlib.h>
#include <string.h>
#include "aether_string.h"

extern const char *aether_merge3(const void *mine, int mine_len,
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

    const char *result = aether_merge3(mine_s, mine_len,
                                        base_s, base_len,
                                        theirs_s, theirs_len);

    string_free(mine_s); string_free(base_s); string_free(theirs_s);

    if (!result) { *out_merged = NULL; *out_len = 0; return -1; }
    int n = (int)aether_string_length(result);
    if (n == 0) { *out_merged = NULL; *out_len = 0; return -1; }   /* error */

    const char *data = aether_string_data(result);
    int status_byte = (unsigned char)data[0];   /* 1 = clean, 2 = conflicts */
    if (status_byte != 1 && status_byte != 2) {
        *out_merged = NULL; *out_len = 0; return -1;
    }
    int payload_len = n - 1;
    char *buf = malloc((size_t)payload_len + 1);
    if (!buf) { *out_merged = NULL; *out_len = 0; return -1; }
    if (payload_len > 0) memcpy(buf, data + 1, (size_t)payload_len);
    buf[payload_len] = '\0';
    *out_merged = buf;
    *out_len    = payload_len;
    return status_byte == 1 ? 0 : 1;
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
