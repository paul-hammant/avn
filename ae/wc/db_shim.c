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

/* ae/wc/db_shim.c — the working-copy metadata database.
 *
 * Each WC has a .svn/wc.db SQLite file. Schema is deliberately narrower
 * than reference svn's wc_db — we track just what the Phase 5 commands
 * actually need. Extensions (tree conflicts, externals, changelists,
 * mergeinfo) land as the surrounding phases need them.
 *
 * Tables:
 *
 *   nodes(path TEXT PK, kind INT, base_rev INT, base_sha1 TEXT, state INT)
 *     kind:     0 = file, 1 = dir
 *     base_rev: revision this node was checked out / updated from
 *     base_sha1:pristine content sha1 (files only; "" for dirs)
 *     state:    0 = normal (tracked, base matches pristine)
 *               1 = added          (scheduled for commit, no base)
 *               2 = deleted        (scheduled for commit, was in base)
 *               3 = replaced       (delete + add against a prior path)
 *
 *   info(key TEXT PK, value TEXT)
 *     Simple KV for "url", "repo_name", "wc_root" and similar globals
 *     we want without a dedicated table.
 *
 * The shim exposes:
 *   open(wc_root)           → handle  -- opens (or creates) .svn/wc.db
 *   close(handle)
 *   upsert_node(...)        -- insert or replace a nodes row
 *   get_node(path)          → handle  -- one-row fetch (nullable)
 *   node_exists(path)       → int
 *   delete_node(path)
 *   list_nodes()            → handle  -- all rows, ordered by path
 *   set_info(key, value)
 *   get_info(key)           → string (empty if missing)
 *
 * Handle types are opaque svnae_ptr types to Aether; accessors pull
 * individual columns.
 */

#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- open / close ----------------------------------------------------- */

/* Given a wc_root like "/home/me/proj", open (or create) .svn/wc.db and
 * install the schema if this is a fresh db. Returns the sqlite handle
 * (as ptr) or NULL. Also mkdir -p's .svn and .svn/pristine for the
 * caller's convenience.
 *
 * Schema install and the Phase 5.13 `conflicted` column migration
 * live in ae/wc/db_schema.ae now; this function does the filesystem
 * setup + sqlite_open and hands off. */
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

/* --- node CRUD -------------------------------------------------------- *
 *
 * svnae_wc_db_upsert_node and svnae_wc_db_get_node moved to
 * ae/wc/db_nodes.ae in round 42. The C side keeps the struct layout,
 * the six accessors, the free function, and one storage primitive —
 * svnae_wc_node_alloc — that Aether's get_node calls with the six
 * row fields it reads off the sqlite statement. */

struct svnae_wc_node {
    char *path;
    int   kind;
    int   base_rev;
    char *base_sha1;
    int   state;
    int   conflicted;
};

struct svnae_wc_node *
svnae_wc_node_alloc(const char *path, int kind, int base_rev,
                     const char *base_sha1, int state, int conflicted)
{
    struct svnae_wc_node *n = calloc(1, sizeof *n);
    if (!n) return NULL;
    n->path       = strdup(path ? path : "");
    n->kind       = kind;
    n->base_rev   = base_rev;
    n->base_sha1  = strdup(base_sha1 ? base_sha1 : "");
    n->state      = state;
    n->conflicted = conflicted;
    return n;
}

const char *svnae_wc_node_path    (const struct svnae_wc_node *n) { return n ? n->path : ""; }
int         svnae_wc_node_kind    (const struct svnae_wc_node *n) { return n ? n->kind : -1; }
int         svnae_wc_node_base_rev(const struct svnae_wc_node *n) { return n ? n->base_rev : -1; }
const char *svnae_wc_node_base_sha1(const struct svnae_wc_node *n){ return n && n->base_sha1 ? n->base_sha1 : ""; }
int         svnae_wc_node_state   (const struct svnae_wc_node *n) { return n ? n->state : -1; }
int         svnae_wc_node_conflicted(const struct svnae_wc_node *n) { return n ? n->conflicted : 0; }

void
svnae_wc_node_free(struct svnae_wc_node *n)
{
    if (!n) return;
    free(n->path);
    free(n->base_sha1);
    free(n);
}

/* --- list_nodes ------------------------------------------------------- *
 *
 * Returns a snapshot of all rows as an array handle + indexed accessors.
 * Ordered by path for deterministic iteration.
 */

/* svnae_wc_db_set_conflicted moved to ae/wc/db_nodes.ae alongside
 * delete/exists — all three are single-statement scalar ops that
 * map cleanly to the subr.sqlite bindings. */

struct svnae_wc_nodelist {
    struct svnae_wc_node *items;
    int n;
    int cap;
};

/* Storage primitives used by ae/wc/db_nodes.ae's Aether-side
 * svnae_wc_db_list_nodes. The Aether side drives the SQL loop and
 * hands each row to append(); this file keeps the struct layout
 * plus the fixed set of accessors read by every downstream caller. */
struct svnae_wc_nodelist *
svnae_wc_nodelist_new(void)
{
    return calloc(1, sizeof(struct svnae_wc_nodelist));
}

int
svnae_wc_nodelist_append(struct svnae_wc_nodelist *L,
                          const char *path, int kind, int base_rev,
                          const char *base_sha1, int state, int conflicted)
{
    if (!L) return -1;
    if (L->n == L->cap) {
        int ncap = L->cap ? L->cap * 2 : 8;
        struct svnae_wc_node *p = realloc(L->items, (size_t)ncap * sizeof *p);
        if (!p) return -1;
        L->items = p;
        L->cap = ncap;
    }
    struct svnae_wc_node *n = &L->items[L->n];
    n->path       = strdup(path ? path : "");
    n->kind       = kind;
    n->base_rev   = base_rev;
    n->base_sha1  = strdup(base_sha1 ? base_sha1 : "");
    n->state      = state;
    n->conflicted = conflicted;
    L->n++;
    return 0;
}

int svnae_wc_nodelist_count(const struct svnae_wc_nodelist *L) { return L ? L->n : 0; }

const char *svnae_wc_nodelist_path    (const struct svnae_wc_nodelist *L, int i) { return (L && i >= 0 && i < L->n) ? L->items[i].path : ""; }
int         svnae_wc_nodelist_kind    (const struct svnae_wc_nodelist *L, int i) { return (L && i >= 0 && i < L->n) ? L->items[i].kind : -1; }
int         svnae_wc_nodelist_base_rev(const struct svnae_wc_nodelist *L, int i) { return (L && i >= 0 && i < L->n) ? L->items[i].base_rev : -1; }
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i){ return (L && i >= 0 && i < L->n && L->items[i].base_sha1) ? L->items[i].base_sha1 : ""; }
int         svnae_wc_nodelist_state   (const struct svnae_wc_nodelist *L, int i) { return (L && i >= 0 && i < L->n) ? L->items[i].state : -1; }
int         svnae_wc_nodelist_conflicted(const struct svnae_wc_nodelist *L, int i) { return (L && i >= 0 && i < L->n) ? L->items[i].conflicted : 0; }

void
svnae_wc_nodelist_free(struct svnae_wc_nodelist *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) {
        free(L->items[i].path);
        free(L->items[i].base_sha1);
    }
    free(L->items);
    free(L);
}

/* --- info kv --------------------------------------------------------- *
 *
 * set_info / get_info moved to ae/wc/db_nodes.ae. The C side keeps
 * svnae_wc_info_dup + svnae_wc_info_free so existing callers that
 * receive a malloc'd char* and later free() it continue to work: the
 * Aether get_info delegates the malloc through dup to hand the caller
 * a pointer detached from the sqlite statement lifetime. */

char *
svnae_wc_info_dup(const char *s)
{
    return strdup(s ? s : "");
}

void svnae_wc_info_free(char *s) { free(s); }
