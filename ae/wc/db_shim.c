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

/* ae/wc/db_shim.c — wc.db lifecycle + opaque handles for the nodes
 * table. SQL drive lives in ae/wc/db_nodes.ae; per-row field accessors
 * are now packed-string handles (subr/packed_handle/) rather than
 * C structs. Public ABI of svnae_wc_node{,list}_* is unchanged —
 * callers see one-line forwards onto svnae_packed_pin_at /
 * svnae_packed_int_at, the same pattern ra/shim.c and repos/shim.c
 * use. */

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "../subr/packed_handle/packed_handle.h"

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

/* --- node handle (single-record) and nodelist handle (packed array) -- *
 *
 * Both shapes are svnae_packed_handle backed. Single-node packs as a
 * lone "<path>\x01<kind>\x01<base_rev>\x01<base_sha1>\x01<state>\x01
 * <conflicted>" record (n=0); nodelist prefixes "<n>\x02" + records
 * separated by \x02. Field accessors live in ae/wc/db_node_packed.ae.
 *
 * Type-safe casts: callers continue to see opaque struct pointers
 * (struct svnae_wc_node, struct svnae_wc_nodelist), but those are
 * just opaque names for svnae_packed_handle. */

/* Field-extractor extern declarations, matching the Aether exports
 * in ae/wc/db_node_packed.ae. */
extern int         aether_wc_node_count       (const char *packed);
extern const char *aether_wc_node_path        (const char *packed);
extern int         aether_wc_node_kind        (const char *packed);
extern int         aether_wc_node_base_rev    (const char *packed);
extern const char *aether_wc_node_base_sha1   (const char *packed);
extern int         aether_wc_node_state       (const char *packed);
extern int         aether_wc_node_conflicted  (const char *packed);
extern const char *aether_wc_node_path_at     (const char *packed, int i);
extern int         aether_wc_node_kind_at     (const char *packed, int i);
extern int         aether_wc_node_base_rev_at (const char *packed, int i);
extern const char *aether_wc_node_base_sha1_at(const char *packed, int i);
extern int         aether_wc_node_state_at    (const char *packed, int i);
extern int         aether_wc_node_conflicted_at(const char *packed, int i);

/* Construct from a Aether-built packed string. The Aether side
 * (db_nodes.ae) builds a single-record packed and a list packed,
 * then calls one of these to wrap into the shared handle type. */
struct svnae_wc_node *
svnae_wc_node_handle(const char *packed)
{
    if (!packed || !*packed) return NULL;
    return (struct svnae_wc_node *)svnae_packed_handle_new(packed, NULL);
}

struct svnae_wc_nodelist *
svnae_wc_nodelist_handle(const char *packed)
{
    if (!packed) return NULL;
    return (struct svnae_wc_nodelist *)svnae_packed_handle_new(packed,
                                                aether_wc_node_count);
}

/* Single-record accessors (info-style). */
const char *svnae_wc_node_path       (const struct svnae_wc_node *n) { return svnae_packed_pin_field((void *)n, aether_wc_node_path); }
int         svnae_wc_node_kind       (const struct svnae_wc_node *n) { return svnae_packed_int_field((const void *)n, aether_wc_node_kind); }
int         svnae_wc_node_base_rev   (const struct svnae_wc_node *n) { return svnae_packed_int_field((const void *)n, aether_wc_node_base_rev); }
const char *svnae_wc_node_base_sha1  (const struct svnae_wc_node *n) { return svnae_packed_pin_field((void *)n, aether_wc_node_base_sha1); }
int         svnae_wc_node_state      (const struct svnae_wc_node *n) { return svnae_packed_int_field((const void *)n, aether_wc_node_state); }
int         svnae_wc_node_conflicted (const struct svnae_wc_node *n) { return svnae_packed_int_field((const void *)n, aether_wc_node_conflicted); }
void        svnae_wc_node_free       (struct svnae_wc_node *n) { svnae_packed_handle_free((struct svnae_packed_handle *)n); }

/* Nodelist accessors. */
int svnae_wc_nodelist_count          (const struct svnae_wc_nodelist *L) { return svnae_packed_count(L); }
const char *svnae_wc_nodelist_path    (const struct svnae_wc_nodelist *L, int i) { return svnae_packed_pin_at((void *)L, i, aether_wc_node_path_at); }
int         svnae_wc_nodelist_kind    (const struct svnae_wc_nodelist *L, int i) { return svnae_packed_int_at(L, i, aether_wc_node_kind_at); }
int         svnae_wc_nodelist_base_rev(const struct svnae_wc_nodelist *L, int i) { return svnae_packed_int_at(L, i, aether_wc_node_base_rev_at); }
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i) { return svnae_packed_pin_at((void *)L, i, aether_wc_node_base_sha1_at); }
int         svnae_wc_nodelist_state   (const struct svnae_wc_nodelist *L, int i) { return svnae_packed_int_at(L, i, aether_wc_node_state_at); }
int         svnae_wc_nodelist_conflicted(const struct svnae_wc_nodelist *L, int i) { return svnae_packed_int_at(L, i, aether_wc_node_conflicted_at); }
void        svnae_wc_nodelist_free   (struct svnae_wc_nodelist *L) { svnae_packed_handle_free((struct svnae_packed_handle *)L); }

/* svnae_wc_info_dup / _free retired in Round 143 — db_nodes.ae now
 * detaches sqlite columns via string.copy (refcounted AetherString)
 * before sqlite_finalize, and svnae_wc_db_get_info returns a
 * `string` instead of a malloc'd `ptr`. */
