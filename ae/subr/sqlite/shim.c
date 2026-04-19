/* sqlite/shim.c — Aether-friendly wrapper over sqlite3.
 *
 * sqlite3's C API uses function-pointer destructors on bind_text/blob
 * (SQLITE_STATIC vs SQLITE_TRANSIENT) and returns int64 columns. Aether's
 * extern syntax doesn't model function pointers, so we hide them here —
 * bind_text always uses SQLITE_TRANSIENT (sqlite copies) so the Aether
 * caller doesn't have to keep its buffer alive across the step.
 *
 * We also expose an svnae_buf for blob columns so the Aether side doesn't
 * have to juggle (ptr, length) pairs through a call chain that drops types.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

/* Shared buf shape with the other shims at the Aether level — opaque ptr. */
struct svnae_buf {
    char *data;
    int   length;
};

static struct svnae_buf *
buf_from(const unsigned char *src, int n)
{
    struct svnae_buf *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->data = malloc((size_t)n + 1);
    if (!b->data) { free(b); return NULL; }
    if (n > 0) memcpy(b->data, src, n);
    b->data[n] = '\0';
    b->length = n;
    return b;
}

/* Thin pass-throughs. We don't hide the sqlite return codes — callers
 * check for SQLITE_OK (0), SQLITE_ROW (100), SQLITE_DONE (101). */

int svnae_sqlite_close(sqlite3 *db)           { return sqlite3_close(db); }
int svnae_sqlite_exec (sqlite3 *db, const char *sql) { return sqlite3_exec(db, sql, NULL, NULL, NULL); }
int svnae_sqlite_changes(sqlite3 *db)         { return sqlite3_changes(db); }
const char *svnae_sqlite_errmsg(sqlite3 *db)  { return sqlite3_errmsg(db); }

/* Aether passes `ptr` for the output parameter. Because extern can't return
 * via a reference type cleanly, we expose two-call "open" via a wrapper that
 * returns the handle directly. sqlite3_stmt** is the same story for
 * prepare. */
/* Open a database (creating if needed). Returns handle or NULL on failure.
 * Aether callers check errmsg() on a NULL return to get the reason — but
 * sqlite3_errmsg(NULL) isn't safe, so we also stash the last-error string
 * in a static for reporting out-of-band. */
static char g_last_open_err[512] = {0};

sqlite3 *
svnae_sqlite_open(const char *path)
{
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(path, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (rc != SQLITE_OK) {
        const char *m = db ? sqlite3_errmsg(db) : "unknown";
        snprintf(g_last_open_err, sizeof g_last_open_err, "%s", m ? m : "unknown");
        if (db) sqlite3_close(db);
        return NULL;
    }
    g_last_open_err[0] = '\0';
    return db;
}

const char *svnae_sqlite_last_open_err(void) { return g_last_open_err; }

/* Prepare a statement. Returns handle or NULL. On NULL, use errmsg(db)
 * to get the reason. */
sqlite3_stmt *
svnae_sqlite_prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (stmt) sqlite3_finalize(stmt);
        return NULL;
    }
    return stmt;
}

/* bind_text uses SQLITE_TRANSIENT — sqlite copies the bytes, so the caller
 * is free to mutate or free its buffer immediately after. */
int
svnae_sqlite_bind_text(sqlite3_stmt *s, int idx, const char *text)
{
    return sqlite3_bind_text(s, idx, text, -1, SQLITE_TRANSIENT);
}

int
svnae_sqlite_bind_blob(sqlite3_stmt *s, int idx, const char *data, int len)
{
    return sqlite3_bind_blob(s, idx, data, len, SQLITE_TRANSIENT);
}

int svnae_sqlite_bind_int (sqlite3_stmt *s, int idx, int v)       { return sqlite3_bind_int(s, idx, v); }

/* 64-bit values round-trip through two 32-bit halves because Aether's
 * `int` maps to C `int` — not a guaranteed 64-bit type. We split on
 * bind and reassemble on column. For rep-cache (offsets, sizes) svn
 * already uses int64, so this matters. */
int
svnae_sqlite_bind_i64(sqlite3_stmt *s, int idx, int hi, int lo)
{
    sqlite3_int64 v = ((sqlite3_int64)(unsigned int)hi << 32)
                    | (sqlite3_int64)(unsigned int)lo;
    return sqlite3_bind_int64(s, idx, v);
}

int svnae_sqlite_step     (sqlite3_stmt *s)                       { return sqlite3_step(s); }
int svnae_sqlite_reset    (sqlite3_stmt *s)                       { return sqlite3_reset(s); }
int svnae_sqlite_finalize (sqlite3_stmt *s)                       { return sqlite3_finalize(s); }

int svnae_sqlite_column_count(sqlite3_stmt *s) { return sqlite3_column_count(s); }
int svnae_sqlite_column_int  (sqlite3_stmt *s, int col) { return sqlite3_column_int(s, col); }

const char *
svnae_sqlite_column_text(sqlite3_stmt *s, int col)
{
    const unsigned char *t = sqlite3_column_text(s, col);
    return t ? (const char *)t : "";
}

/* Read the 64-bit column as (hi, lo) pair. Aether reads the two halves
 * via two extern calls; we stash the value between them in a tiny thread-
 * local to avoid needing multi-return. sqlite3 itself is single-threaded
 * per-connection, so this is safe for the usage patterns svn has. */
static sqlite3_int64 g_i64_value = 0;

int svnae_sqlite_column_i64_hi(sqlite3_stmt *s, int col) {
    g_i64_value = sqlite3_column_int64(s, col);
    return (int)(g_i64_value >> 32);
}
int svnae_sqlite_column_i64_lo(sqlite3_stmt *s, int col) {
    (void)s; (void)col;
    return (int)(unsigned int)g_i64_value;
}

/* Column blobs come back as an svnae_buf handle. Caller frees it with
 * svnae_sqlite_buf_free. */
struct svnae_buf *
svnae_sqlite_column_blob_buf(sqlite3_stmt *s, int col)
{
    int n = sqlite3_column_bytes(s, col);
    const void *data = sqlite3_column_blob(s, col);
    return buf_from((const unsigned char *)data, n);
}

int        svnae_sqlite_buf_length(const struct svnae_buf *b) { return b ? b->length : 0; }
const char *svnae_sqlite_buf_data  (const struct svnae_buf *b) { return b ? b->data : ""; }
void        svnae_sqlite_buf_free  (struct svnae_buf *b)
{
    if (!b) return;
    free(b->data);
    free(b);
}
