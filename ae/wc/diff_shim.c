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

/* ae/wc/diff_shim.c — binary tmp-write adapter for svn diff.
 *
 * Round 80 (Gordian) ported svnae_wc_diff orchestration to
 * ae/wc/diff.ae. fork+execl /usr/bin/diff goes through std.os.os_run;
 * tmp-file staging via fs.write_atomic. What stays in C: the
 * pristine-bytes-to-tmp-file path, because svnae_wc_pristine_get
 * returns malloc'd `char *` that may contain embedded NULs (and
 * Aether's `string` ABI would truncate at the first one).
 */

#include <stdlib.h>
#include <string.h>

extern int   svnae_wc_pristine_size(const char *wc_root, const char *sha);
extern char *svnae_wc_pristine_get (const char *wc_root, const char *sha);
extern void  svnae_wc_pristine_free(char *p);
extern int   aether_io_write_atomic(const char *path, const char *data, int length);

/* Slurp pristine `sha` into a tmp file at `path`. Returns 0 on
 * success, -1 on any failure (pristine miss or write error). */
int
svnae_diff_tmp_write_pristine(const char *wc_root, const char *sha,
                              const char *path)
{
    int sz = svnae_wc_pristine_size(wc_root, sha);
    char *data = svnae_wc_pristine_get(wc_root, sha);
    if (!data) return -1;
    int rc = aether_io_write_atomic(path, data, sz);
    svnae_wc_pristine_free(data);
    return rc == 0 ? 0 : -1;
}
