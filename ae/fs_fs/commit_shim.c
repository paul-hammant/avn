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

/* fs_fs/commit_shim.c — fnmatch(3) FFI for ae/fs_fs/spec.ae.
 *
 * The historical svnae_branch_spec_allows null-guarded wrapper was
 * retired in Round 115 — its sole C caller (svnserver_spec_allows
 * in svnserver/shim.c) inlines the null guard and calls the Aether
 * export aether_branch_spec_allows directly. */

#include <fnmatch.h>

int
svnae_fnmatch_pathname(const char *glob, const char *path)
{
    return fnmatch(glob, path, FNM_PATHNAME) == 0 ? 1 : 0;
}
