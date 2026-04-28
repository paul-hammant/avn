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

/* repos/shim.c — libsvn_repos query surface, slimmed.
 *
 * Round 154 hoisted the log/list/info/paths/blame accessor
 * families to ae/repos/accessors.ae. The Aether-side handles
 * are now just refcounted AetherStrings holding the packed-
 * record buffer; the pin_str / packed_handle dance is gone
 * because Aether's refcount upholds the stable-pointer contract
 * those families needed.
 *
 * What remains here: svnae_repos_cat — the binary-blob slurp
 * still allocates a malloc'd char* for the existing C-ABI
 * caller (Aether's `string` would NUL-truncate at the FFI
 * boundary). */

#include <stdlib.h>
#include <string.h>

extern char *svnae_rep_read_blob(const char *repo, const char *sha1_hex);

extern const char *aether_repos_load_rev_blob_field(const char *repo, int rev,
                                                    const char *key);
extern int         aether_resolve_kind(const char *repo, const char *root_sha, const char *path);
extern const char *aether_resolve_sha (const char *repo, const char *root_sha, const char *path);

/* Read file content at (rev, path). Returns malloc'd bytes or NULL —
 * caller frees via svnae_rep_free. Stays on the C side because the
 * payload may contain embedded NULs and Aether's `string` ABI would
 * truncate at the first one on the FFI boundary. */
char *
svnae_repos_cat(const char *repo, int rev, const char *path)
{
    const char *root = aether_repos_load_rev_blob_field(repo, rev, "root");
    if (!root || !*root) return NULL;
    /* Take a copy now — the Aether helper hands back a pointer the
     * runtime is free to reuse on the next call. */
    char root_buf[65];
    size_t rlen = strlen(root);
    if (rlen >= sizeof root_buf) return NULL;
    memcpy(root_buf, root, rlen + 1);

    int k = aether_resolve_kind(repo, root_buf, path);
    if (k != 'f') return NULL;
    const char *sha = aether_resolve_sha(repo, root_buf, path);
    if (!sha || !*sha) return NULL;
    return svnae_rep_read_blob(repo, sha);
}
