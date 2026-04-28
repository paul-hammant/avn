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

/* ae/wc/db_shim.c — wc.db lifecycle (open / close). The node +
 * nodelist accessor families moved to ae/wc/db_accessors.ae in
 * Round 156; the SQL driver lives in ae/wc/db_nodes.ae. */

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- open / close ----------------------------------------------------- */

extern int aether_wc_db_install_schema(sqlite3 *db);

sqlite3 *
svnae_wc_db_open(const char *wc_root)
{
    extern int aether_io_mkdir_p(const char *path);
    char pristine_dir[PATH_MAX];
    snprintf(pristine_dir, sizeof pristine_dir, "%s/.svn/pristine", wc_root);
    if (aether_io_mkdir_p(pristine_dir) != 0) return NULL;

    char dbpath[PATH_MAX];
    snprintf(dbpath, sizeof dbpath, "%s/.svn/wc.db", wc_root);

    sqlite3 *db;
    if (sqlite3_open_v2(dbpath, &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    if (aether_wc_db_install_schema(db) != 0) {
        sqlite3_close(db);
        return NULL;
    }
    return db;
}

void svnae_wc_db_close(sqlite3 *db) { if (db) sqlite3_close(db); }

/* node + nodelist handle accessor families retired in Round 156 —
 * moved to ae/wc/db_accessors.ae. Same recipe as Rounds 154 / 155
 * (repos / ra accessors). */

/* svnae_wc_info_dup / _free retired in Round 143 — db_nodes.ae now
 * detaches sqlite columns via string.copy (refcounted AetherString)
 * before sqlite_finalize, and svnae_wc_db_get_info returns a
 * `string` instead of a malloc'd `ptr`. */
