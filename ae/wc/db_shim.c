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
 * caller's convenience. */
sqlite3 *
svnae_wc_db_open(const char *wc_root)
{
    /* mkdir $wc_root/.svn and $wc_root/.svn/pristine */
    char svn_dir[PATH_MAX];
    snprintf(svn_dir, sizeof svn_dir, "%s/.svn", wc_root);
    mkdir(wc_root, 0755);                 /* WC root must already exist; ignore EEXIST */
    if (mkdir(svn_dir, 0755) != 0 && errno != EEXIST) return NULL;
    char pristine_dir[PATH_MAX];
    snprintf(pristine_dir, sizeof pristine_dir, "%s/pristine", svn_dir);
    if (mkdir(pristine_dir, 0755) != 0 && errno != EEXIST) return NULL;

    char dbpath[PATH_MAX];
    snprintf(dbpath, sizeof dbpath, "%s/wc.db", svn_dir);

    sqlite3 *db;
    if (sqlite3_open_v2(dbpath, &db,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    /* Install schema if absent. CREATE TABLE IF NOT EXISTS keeps this
     * idempotent across re-opens. */
    const char *schema =
        "CREATE TABLE IF NOT EXISTS nodes ("
        "  path       TEXT PRIMARY KEY,"
        "  kind       INT NOT NULL,"
        "  base_rev   INT NOT NULL,"
        "  base_sha1  TEXT NOT NULL,"
        "  state      INT NOT NULL,"
        "  conflicted INT NOT NULL DEFAULT 0"
        ");"
        "CREATE TABLE IF NOT EXISTS info ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");";
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    /* Migration for wc.db files created before Phase 5.13: if the
     * `conflicted` column is missing, add it with a default of 0.
     * ALTER TABLE ADD COLUMN fails if the column already exists — we
     * check pragma table_info rather than trusting the error. */
    sqlite3_stmt *pc = NULL;
    int has_conflicted = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(nodes)", -1, &pc, NULL) == SQLITE_OK) {
        while (sqlite3_step(pc) == SQLITE_ROW) {
            const unsigned char *nm = sqlite3_column_text(pc, 1);
            if (nm && strcmp((const char *)nm, "conflicted") == 0) { has_conflicted = 1; break; }
        }
        sqlite3_finalize(pc);
    }
    if (!has_conflicted) {
        sqlite3_exec(db, "ALTER TABLE nodes ADD COLUMN conflicted INT NOT NULL DEFAULT 0",
                     NULL, NULL, NULL);
    }
    return db;
}

void svnae_wc_db_close(sqlite3 *db) { if (db) sqlite3_close(db); }

/* --- node CRUD -------------------------------------------------------- */

/* Insert or replace a node row. Returns 0 on success.
 *
 * Aether v0.70.0 (issue #16) truncates extern declarations at 5 params.
 * We pack `kind` (0..1) and `state` (0..3) into a single int as
 *   kind_state = (state << 4) | kind
 * so the call-site only needs 5 Aether args. The C-level function
 * takes them unpacked for clarity. */
int
svnae_wc_db_upsert_node(sqlite3 *db,
                        const char *path, int kind, int base_rev,
                        const char *base_sha1, int state)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO nodes (path, kind, base_rev, base_sha1, state) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "  kind=excluded.kind, base_rev=excluded.base_rev,"
        "  base_sha1=excluded.base_sha1, state=excluded.state";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 2, kind);
    sqlite3_bind_int (st, 3, base_rev);
    sqlite3_bind_text(st, 4, base_sha1 ? base_sha1 : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int (st, 5, state);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* 5-arg wrapper for Aether. `ks` packs (state << 4) | kind; this matches
 * the issue-#16 workaround. */
int
svnae_wc_db_upsert_node_packed(sqlite3 *db,
                               const char *path, int ks, int base_rev,
                               const char *base_sha1)
{
    int kind  = ks & 0xf;
    int state = (ks >> 4) & 0xf;
    return svnae_wc_db_upsert_node(db, path, kind, base_rev, base_sha1, state);
}

int
svnae_wc_db_delete_node(sqlite3 *db, const char *path)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM nodes WHERE path = ?",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

int
svnae_wc_db_node_exists(sqlite3 *db, const char *path)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM nodes WHERE path = ?",
                           -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_ROW ? 1 : 0;
}

/* get_node returns a tiny struct as a ptr handle; Aether reads fields
 * through accessors. Returns NULL if no such row. */

struct svnae_wc_node {
    char *path;
    int   kind;
    int   base_rev;
    char *base_sha1;
    int   state;
    int   conflicted;
};

struct svnae_wc_node *
svnae_wc_db_get_node(sqlite3 *db, const char *path)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "SELECT path, kind, base_rev, base_sha1, state, conflicted"
        "  FROM nodes WHERE path = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) { sqlite3_finalize(st); return NULL; }

    struct svnae_wc_node *n = calloc(1, sizeof *n);
    n->path      = strdup((const char *)sqlite3_column_text(st, 0));
    n->kind      = sqlite3_column_int(st, 1);
    n->base_rev  = sqlite3_column_int(st, 2);
    const char *s = (const char *)sqlite3_column_text(st, 3);
    n->base_sha1 = strdup(s ? s : "");
    n->state     = sqlite3_column_int(st, 4);
    n->conflicted = sqlite3_column_int(st, 5);
    sqlite3_finalize(st);
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

/* Set the conflicted flag on a node without touching other fields.
 * Returns 0 on success. */
int
svnae_wc_db_set_conflicted(sqlite3 *db, const char *path, int conflicted)
{
    sqlite3_stmt *st = NULL;
    const char *sql = "UPDATE nodes SET conflicted = ? WHERE path = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int (st, 1, conflicted);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

struct svnae_wc_nodelist {
    struct svnae_wc_node *items;
    int n;
};

struct svnae_wc_nodelist *
svnae_wc_db_list_nodes(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    const char *count_sql = "SELECT COUNT(*) FROM nodes";
    if (sqlite3_prepare_v2(db, count_sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    int n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    struct svnae_wc_nodelist *L = calloc(1, sizeof *L);
    L->n = n;
    L->items = calloc((size_t)n, sizeof *L->items);

    const char *sql =
        "SELECT path, kind, base_rev, base_sha1, state, conflicted"
        "  FROM nodes ORDER BY path";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        free(L->items); free(L); return NULL;
    }
    int i = 0;
    while (sqlite3_step(st) == SQLITE_ROW && i < n) {
        L->items[i].path       = strdup((const char *)sqlite3_column_text(st, 0));
        L->items[i].kind       = sqlite3_column_int(st, 1);
        L->items[i].base_rev   = sqlite3_column_int(st, 2);
        const char *s          = (const char *)sqlite3_column_text(st, 3);
        L->items[i].base_sha1  = strdup(s ? s : "");
        L->items[i].state      = sqlite3_column_int(st, 4);
        L->items[i].conflicted = sqlite3_column_int(st, 5);
        i++;
    }
    L->n = i;   /* in case count raced with stepping */
    sqlite3_finalize(st);
    return L;
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

/* --- info kv --------------------------------------------------------- */

int
svnae_wc_db_set_info(sqlite3 *db, const char *key, const char *value)
{
    sqlite3_stmt *st = NULL;
    const char *sql =
        "INSERT INTO info (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(st, 1, key,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* get_info returns a malloc'd copy (caller frees with svnae_wc_info_free)
 * or NULL if missing. */
char *
svnae_wc_db_get_info(sqlite3 *db, const char *key)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM info WHERE key = ?",
                           -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *s = (const char *)sqlite3_column_text(st, 0);
        out = strdup(s ? s : "");
    }
    sqlite3_finalize(st);
    return out;
}

void svnae_wc_info_free(char *s) { free(s); }
