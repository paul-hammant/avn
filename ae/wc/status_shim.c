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

/* ae/wc/status_shim.c — fnmatch(3) FFI helper for the ignore-rule
 * matcher. The statuslist accessor family moved to
 * ae/wc/wc_accessors.ae in Round 157. */

#include <fnmatch.h>
#include <stdint.h>

/* FFI helper for ae/wc/ignore.ae — plain fnmatch(3) without
 * FNM_PATHNAME, matching svn:ignore's semantics (basename-style
 * patterns; `*` may span `/` characters if the caller ever passes a
 * multi-segment name). */
int32_t
svnae_fnmatch_plain(const char *glob, const char *path)
{
    return fnmatch(glob, path, 0) == 0 ? 1 : 0;
}
