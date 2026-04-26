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

/* fs_fs/commit_shim.c — leaf bindings for commit finalise.
 *
 * Round 64 (Gordian knot) ported the orchestration of
 * svnae_commit_finalise* and svnae_branch_create to
 * ae/fs_fs/commit_finalise.ae. What remains here:
 *
 *   - svnae_fnmatch_pathname  (fnmatch(3) FFI for ae/fs_fs/spec.ae)
 *   - svnae_branch_spec_allows (one-line trampoline for C callers
 *     that link without the Aether export wrapper — currently still
 *     called from ae/svnserver/shim.c).
 *
 * Every other function in this file pre-Round 64 was orchestration
 * (recursive tree-rebuild stays in txn_shim.c — it's the actual leaf
 * of the commit pipeline; everything else was just plumbing).
 */

#include <fnmatch.h>

/* FFI helper the Aether spec matcher (ae/fs_fs/spec.ae) calls. Keeps
 * fnmatch(3) on the C side since Aether has no binding for it. */
int
svnae_fnmatch_pathname(const char *glob, const char *path)
{
    return fnmatch(glob, path, FNM_PATHNAME) == 0 ? 1 : 0;
}

/* Trampoline preserved for C callers (svnserver/shim.c reaches it
 * via a C-symbol declaration). The Aether export is named
 * aether_branch_spec_allows; this thin wrapper keeps the
 * svnae_branch_spec_allows symbol stable. */
extern int aether_branch_spec_allows(const char *repo, const char *branch,
                                     const char *path);
int
svnae_branch_spec_allows(const char *repo, const char *branch, const char *path)
{
    if (!repo || !branch || !path) return -1;
    return aether_branch_spec_allows(repo, branch, path);
}
