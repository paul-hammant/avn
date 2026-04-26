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

/* fs_fs/commit_shim.c — two leaf bindings:
 *  - svnae_fnmatch_pathname: fnmatch(3) FFI for ae/fs_fs/spec.ae
 *  - svnae_branch_spec_allows: thin null-guarded wrapper kept stable
 *    for C callers that don't link the Aether export wrapper. */

#include <fnmatch.h>

int
svnae_fnmatch_pathname(const char *glob, const char *path)
{
    return fnmatch(glob, path, FNM_PATHNAME) == 0 ? 1 : 0;
}

extern int aether_branch_spec_allows(const char *repo, const char *branch,
                                     const char *path);
int
svnae_branch_spec_allows(const char *repo, const char *branch, const char *path)
{
    if (!repo || !branch || !path) return -1;
    return aether_branch_spec_allows(repo, branch, path);
}
