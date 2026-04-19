/* ae/svnadmin/shim.c — svnadmin create / dump / load.
 *
 * Operates on repos in the on-disk shape the rest of the port uses:
 *
 *   $repo/
 *     format              -- "svnae-fsfs-1\n"
 *     revs/NNNNNN         -- "<sha1>\n" pointers per revision
 *     head                -- "<N>\n"
 *     reps/aa/bb/*.rep    -- content-addressable blobs (header byte + payload)
 *     rep-cache.db        -- SQLite: hash/rel_path/uncompressed_size/storage
 *
 * Dump stream (own format; not reference-svn compatible):
 *
 *   SVNAE-DUMP 1\n
 *   FORMAT <string>\n
 *   HEAD <int>\n
 *   REV-COUNT <int>\n
 *   REP-COUNT <int>\n
 *
 *   (REP-COUNT blocks, one per content blob:)
 *     REP <40-char sha1>\n
 *     SIZE <uncompressed-bytes>\n
 *     <uncompressed-bytes> raw bytes
 *     \n
 *
 *   (REV-COUNT blocks, one per revision 0..HEAD:)
 *     REV <n>\n
 *     POINTER <40-char sha1 of the rev's rep>\n
 *
 *   END\n
 *
 * Load rebuilds the target repo from scratch: initialises the layout,
 * writes every REP through the same code path svnae_rep_write_blob uses
 * (so rep-cache.db is populated correctly + compression reapplied), then
 * writes each revs/NNNNNN pointer, then head.
 *
 * No cross-file delta compression of blobs in the stream. Keeps load
 * single-pass. Size is fine for our scale.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Externs from other shims. */
const char *svnae_rep_write_blob(const char *repo, const char *data, int len);
char       *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
void        svnae_rep_free(char *p);
int         svnae_repos_head_rev(const char *repo);
const char *svnae_fsfs_now_iso8601(void);

/* ---- tiny helpers --------------------------------------------------- */

static int
mkdir_p(const char *path)
{
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int
write_file_atomic(const char *path, const char *data, int len)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -errno;
    const char *p = data; int rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -errno; }
        p += w; rem -= (int)w;
    }
    if (fsync(fd) != 0) { int rc = -errno; close(fd); unlink(tmp); return rc; }
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return -errno; }
    return 0;
}

static char *
slurp_small(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    char *buf = malloc((size_t)st.st_size + 1);
    ssize_t got = 0;
    while (got < st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)(st.st_size - got));
        if (n < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (n == 0) break;
        got += n;
    }
    close(fd);
    if (got != st.st_size) { free(buf); return NULL; }
    buf[got] = '\0';
    return buf;
}

static int
parse_int(const char *s, int *out)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s) return 0;
    *out = (int)v;
    return 1;
}

/* ---- create --------------------------------------------------------- */

/* Create the empty on-disk layout: dirs, format file, rep-cache schema.
 * No rev 0, no head. Shared by `create` (which goes on to seed rev 0)
 * and `load` (which replays revs from a dump). */
static int
create_bare(const char *repo)
{
    if (mkdir_p(repo) != 0) return -1;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/reps", repo);
    if (mkdir_p(path) != 0) return -1;
    snprintf(path, sizeof path, "%s/revs", repo);
    if (mkdir_p(path) != 0) return -1;

    snprintf(path, sizeof path, "%s/format", repo);
    if (write_file_atomic(path, "svnae-fsfs-1\n", 13) != 0) return -1;

    snprintf(path, sizeof path, "%s/rep-cache.db", repo);
    sqlite3 *db;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    const char *schema =
        "CREATE TABLE IF NOT EXISTS rep_cache ("
        "  hash TEXT PRIMARY KEY,"
        "  rel_path TEXT NOT NULL,"
        "  uncompressed_size INT NOT NULL,"
        "  storage INT NOT NULL)";
    int rc = sqlite3_exec(db, schema, NULL, NULL, NULL);
    sqlite3_close(db);
    if (rc != SQLITE_OK) return -1;
    return 0;
}

int
svnae_svnadmin_create(const char *repo)
{
    if (create_bare(repo) != 0) return -1;

    /* Seed rev 0: empty root dir + rev blob pointing at it. */
    const char *empty_sha = svnae_rep_write_blob(repo, "", 0);
    if (!empty_sha) return -1;
    char empty_copy[41];
    memcpy(empty_copy, empty_sha, 40);
    empty_copy[40] = '\0';

    char rev0[512];
    int rev0_len = snprintf(rev0, sizeof rev0,
        "root: %s\nprev: 0\nauthor: (init)\ndate: %s\nlog: initial empty revision\n",
        empty_copy, svnae_fsfs_now_iso8601());
    const char *rev0_sha = svnae_rep_write_blob(repo, rev0, rev0_len);
    if (!rev0_sha) return -1;

    char ptr_body[64];
    int plen = snprintf(ptr_body, sizeof ptr_body, "%s\n", rev0_sha);
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/revs/000000", repo);
    if (write_file_atomic(path, ptr_body, plen) != 0) return -1;

    snprintf(path, sizeof path, "%s/head", repo);
    if (write_file_atomic(path, "0\n", 2) != 0) return -1;
    return 0;
}

/* ---- rep enumeration ------------------------------------------------ *
 *
 * Walk rep-cache.db to get every (sha1, uncompressed_size) we've stored.
 * Caller iterates by index. */

struct svnae_rep_list {
    char **sha1s;
    int   *sizes;
    int    n, cap;
};

static struct svnae_rep_list *
list_reps(const char *repo)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/rep-cache.db", repo);
    sqlite3 *db;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT hash, uncompressed_size FROM rep_cache ORDER BY hash",
            -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    struct svnae_rep_list *L = calloc(1, sizeof *L);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (L->n == L->cap) {
            int nc = L->cap ? L->cap * 2 : 32;
            L->sha1s = realloc(L->sha1s, (size_t)nc * sizeof *L->sha1s);
            L->sizes = realloc(L->sizes, (size_t)nc * sizeof *L->sizes);
            L->cap = nc;
        }
        L->sha1s[L->n] = strdup((const char *)sqlite3_column_text(st, 0));
        L->sizes[L->n] = sqlite3_column_int(st, 1);
        L->n++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    return L;
}

static void
free_rep_list(struct svnae_rep_list *L)
{
    if (!L) return;
    for (int i = 0; i < L->n; i++) free(L->sha1s[i]);
    free(L->sha1s); free(L->sizes); free(L);
}

/* ---- dump ----------------------------------------------------------- */

/* Write whole buffer to an fd, retrying on EINTR. */
static int
write_all(int fd, const char *data, int len)
{
    const char *p = data; int rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; rem -= (int)w;
    }
    return 0;
}

int
svnae_svnadmin_dump(const char *repo, int out_fd)
{
    int head = svnae_repos_head_rev(repo);
    if (head < 0) return -1;

    struct svnae_rep_list *reps = list_reps(repo);
    if (!reps) return -1;

    char hdr[256];
    int hlen;
    hlen = snprintf(hdr, sizeof hdr,
        "SVNAE-DUMP 1\nFORMAT svnae-fsfs-1\nHEAD %d\nREV-COUNT %d\nREP-COUNT %d\n",
        head, head + 1, reps->n);
    if (write_all(out_fd, hdr, hlen) != 0) { free_rep_list(reps); return -1; }

    /* Each rep block. */
    for (int i = 0; i < reps->n; i++) {
        char *bytes = svnae_rep_read_blob(repo, reps->sha1s[i]);
        if (!bytes) { free_rep_list(reps); return -1; }

        hlen = snprintf(hdr, sizeof hdr, "REP %s\nSIZE %d\n", reps->sha1s[i], reps->sizes[i]);
        if (write_all(out_fd, hdr, hlen) != 0) { svnae_rep_free(bytes); free_rep_list(reps); return -1; }
        if (write_all(out_fd, bytes, reps->sizes[i]) != 0) { svnae_rep_free(bytes); free_rep_list(reps); return -1; }
        if (write_all(out_fd, "\n", 1) != 0) { svnae_rep_free(bytes); free_rep_list(reps); return -1; }
        svnae_rep_free(bytes);
    }

    /* Rev pointers. */
    for (int r = 0; r <= head; r++) {
        char rp[PATH_MAX];
        snprintf(rp, sizeof rp, "%s/revs/%06d", repo, r);
        char *body = slurp_small(rp);
        if (!body) { free_rep_list(reps); return -1; }
        size_t n = strlen(body);
        while (n > 0 && (body[n-1] == '\n' || body[n-1] == '\r')) body[--n] = '\0';

        hlen = snprintf(hdr, sizeof hdr, "REV %d\nPOINTER %s\n", r, body);
        free(body);
        if (write_all(out_fd, hdr, hlen) != 0) { free_rep_list(reps); return -1; }
    }

    if (write_all(out_fd, "END\n", 4) != 0) { free_rep_list(reps); return -1; }
    free_rep_list(reps);
    return 0;
}

/* ---- load ----------------------------------------------------------- */

/* Read a single line from fd into buf (up to bufsz-1 chars + NUL). Returns
 * length or -1 on error / EOF. Strips trailing '\n'. */
static int
read_line(int fd, char *buf, int bufsz)
{
    int i = 0;
    while (i < bufsz - 1) {
        char c;
        ssize_t n = read(fd, &c, 1);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { if (i == 0) return -1; break; }
        if (c == '\n') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* Read exactly `len` bytes. Returns 0 on success. */
static int
read_n(int fd, char *buf, int len)
{
    int got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, (size_t)(len - got));
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) return -1;
        got += (int)n;
    }
    return 0;
}

int
svnae_svnadmin_load(const char *repo, int in_fd)
{
    char line[512];
    int len;

    /* Header. */
    len = read_line(in_fd, line, sizeof line);
    if (len < 0 || strcmp(line, "SVNAE-DUMP 1") != 0) return -1;
    len = read_line(in_fd, line, sizeof line);   /* FORMAT ... */
    if (len < 0 || strncmp(line, "FORMAT ", 7) != 0) return -1;
    len = read_line(in_fd, line, sizeof line);   /* HEAD ... */
    int head;
    if (len < 0 || strncmp(line, "HEAD ", 5) != 0 || !parse_int(line + 5, &head)) return -1;
    len = read_line(in_fd, line, sizeof line);   /* REV-COUNT ... */
    int rev_count;
    if (len < 0 || strncmp(line, "REV-COUNT ", 10) != 0 || !parse_int(line + 10, &rev_count)) return -1;
    len = read_line(in_fd, line, sizeof line);   /* REP-COUNT ... */
    int rep_count;
    if (len < 0 || strncmp(line, "REP-COUNT ", 10) != 0 || !parse_int(line + 10, &rep_count)) return -1;

    /* Initialise target repo's bare layout (no seed rev 0 — the dump
     * will replay its own rev 0 below). */
    if (create_bare(repo) != 0) return -1;

    /* REP-COUNT blocks. */
    for (int i = 0; i < rep_count; i++) {
        len = read_line(in_fd, line, sizeof line);
        if (len < 0 || strncmp(line, "REP ", 4) != 0) return -1;
        char sha[41]; strncpy(sha, line + 4, 40); sha[40] = '\0';

        len = read_line(in_fd, line, sizeof line);
        if (len < 0 || strncmp(line, "SIZE ", 5) != 0) return -1;
        int size;
        if (!parse_int(line + 5, &size)) return -1;

        char *data = malloc((size_t)size + 1);
        if (!data) return -1;
        if (read_n(in_fd, data, size) != 0) { free(data); return -1; }
        data[size] = '\0';

        /* Trailing newline separator. */
        char nl;
        if (read_n(in_fd, &nl, 1) != 0 || nl != '\n') { free(data); return -1; }

        const char *written = svnae_rep_write_blob(repo, data, size);
        free(data);
        if (!written || strcmp(written, sha) != 0) {
            /* Hash mismatch → dump corrupt. */
            return -1;
        }
    }

    /* REV-COUNT blocks. */
    for (int i = 0; i < rev_count; i++) {
        len = read_line(in_fd, line, sizeof line);
        if (len < 0 || strncmp(line, "REV ", 4) != 0) return -1;
        int rev;
        if (!parse_int(line + 4, &rev)) return -1;

        len = read_line(in_fd, line, sizeof line);
        if (len < 0 || strncmp(line, "POINTER ", 8) != 0) return -1;
        const char *sha = line + 8;

        char ptr_body[64];
        int plen = snprintf(ptr_body, sizeof ptr_body, "%s\n", sha);
        char ptr_path[PATH_MAX];
        snprintf(ptr_path, sizeof ptr_path, "%s/revs/%06d", repo, rev);
        if (write_file_atomic(ptr_path, ptr_body, plen) != 0) return -1;
    }

    /* END + head. */
    len = read_line(in_fd, line, sizeof line);
    if (len < 0 || strcmp(line, "END") != 0) return -1;

    char head_body[32];
    int hlen = snprintf(head_body, sizeof head_body, "%d\n", head);
    char head_path[PATH_MAX];
    snprintf(head_path, sizeof head_path, "%s/head", repo);
    if (write_file_atomic(head_path, head_body, hlen) != 0) return -1;

    return 0;
}
