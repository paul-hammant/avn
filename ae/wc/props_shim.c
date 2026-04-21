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

/* ae/wc/props_shim.c — WC-side properties.
 *
 * For Phase 5.10 properties live only in the working copy. They're
 * stored in wc.db's `props` table. Later phases will extend commit to
 * push them to the server via an extended edit-list op.
 *
 * Table:
 *   props(path TEXT, name TEXT, value TEXT, PRIMARY KEY(path, name))
 *
 * API:
 *   propset(wc_root, path, name, value)           -> 0
 *   propget(wc_root, path, name) -> value         (empty if unset)
 *   propdel(wc_root, path, name)                  -> 0
 *   proplist(wc_root, path)                       -> handle
 *     proplist_count, proplist_name(L, i), proplist_value(L, i)
 *     proplist_free
 *
 * The path must be tracked. Dirs and files both accept props. The
 * `svn:*` namespace is recognised by consumers (Phase 5.11 uses
 * `svn:ignore`) but this shim enforces no naming rules.
 */

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);
int      svnae_wc_db_node_exists(sqlite3 *db, const char *path);

/* --- one-time schema migration ---------------------------------------- */

static int
ensure_schema(sqlite3 *db)
{
    const char *sql =
        "CREATE TABLE IF NOT EXISTS props ("
        "  path  TEXT NOT NULL,"
        "  name  TEXT NOT NULL,"
        "  value TEXT NOT NULL,"
        "  PRIMARY KEY (path, name))";
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/* --- propset ---------------------------------------------------------- */

int
svnae_wc_propset(const char *wc_root, const char *path,
                 const char *name, const char *value)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    if (ensure_schema(db) != 0) { svnae_wc_db_close(db); return -1; }
    if (!svnae_wc_db_node_exists(db, path)) {
        svnae_wc_db_close(db);
        return -2;  /* path not tracked */
    }

    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO props (path, name, value) VALUES (?, ?, ?) "
        "ON CONFLICT(path, name) DO UPDATE SET value=excluded.value";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        svnae_wc_db_close(db); return -1;
    }
    sqlite3_bind_text(st, 1, path,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    svnae_wc_db_close(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* --- propget ---------------------------------------------------------- *
 *
 * Returns a malloc'd value, or NULL if unset. Caller frees with
 * svnae_wc_props_free. */

char *
svnae_wc_propget(const char *wc_root, const char *path, const char *name)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return NULL;
    if (ensure_schema(db) != 0) { svnae_wc_db_close(db); return NULL; }

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT value FROM props WHERE path=? AND name=?",
            -1, &st, NULL) != SQLITE_OK) {
        svnae_wc_db_close(db); return NULL;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);

    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        out = strdup(s ? s : "");
    }
    sqlite3_finalize(st);
    svnae_wc_db_close(db);
    return out;
}

void svnae_wc_props_free(char *s) { free(s); }

/* --- propdel ---------------------------------------------------------- */

int
svnae_wc_propdel(const char *wc_root, const char *path, const char *name)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;
    if (ensure_schema(db) != 0) { svnae_wc_db_close(db); return -1; }

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM props WHERE path=? AND name=?",
            -1, &st, NULL) != SQLITE_OK) {
        svnae_wc_db_close(db); return -1;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    svnae_wc_db_close(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* --- proplist --------------------------------------------------------- */

struct prop_entry { char *name; char *value; };
struct svnae_wc_proplist { struct prop_entry *items; int n; };

struct svnae_wc_proplist *
svnae_wc_proplist(const char *wc_root, const char *path)
{
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return NULL;
    if (ensure_schema(db) != 0) { svnae_wc_db_close(db); return NULL; }

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT name, value FROM props WHERE path=? ORDER BY name",
            -1, &st, NULL) != SQLITE_OK) {
        svnae_wc_db_close(db); return NULL;
    }
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);

    struct svnae_wc_proplist *L = calloc(1, sizeof *L);
    int cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (L->n == cap) {
            cap = cap ? cap * 2 : 8;
            L->items = realloc(L->items, (size_t)cap * sizeof *L->items);
        }
        L->items[L->n].name  = strdup((const char *)sqlite3_column_text(st, 0));
        L->items[L->n].value = strdup((const char *)sqlite3_column_text(st, 1));
        L->n++;
    }
    sqlite3_finalize(st);
    svnae_wc_db_close(db);
    return L;
}

int svnae_wc_proplist_count(const struct svnae_wc_proplist *L) { return L ? L->n : 0; }

const char *
svnae_wc_proplist_name(const struct svnae_wc_proplist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].name;
}

const char *
svnae_wc_proplist_value(const struct svnae_wc_proplist *L, int i)
{
    if (!L || i < 0 || i >= L->n) return "";
    return L->items[i].value;
}

void
svnae_wc_proplist_free(struct svnae_wc_proplist *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) { free(L->items[i].name); free(L->items[i].value); }
    free(L->items);
    free(L);
}
