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

/* utf8/shim.c — thin wrapper around libutf8proc's NFC normalization.
 *
 * svn normalizes internal paths to NFC so that macOS (which stores them
 * NFD on HFS+/APFS) round-trips correctly against Linux/Windows. We'll
 * call svnae_utf8_nfc at the boundaries where we first receive a path
 * from the OS (readdir, argv, environment) and trust the internal form
 * everywhere else.
 */

#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

/* Returns a newly-malloc'd NUL-terminated NFC form of `s`, or NULL on error.
 * Caller frees via svnae_utf8_free. */
char *
svnae_utf8_nfc(const char *s)
{
    utf8proc_uint8_t *out = utf8proc_NFC((const utf8proc_uint8_t *)s);
    return (char *)out;
}

void svnae_utf8_free(char *p) { free(p); }
