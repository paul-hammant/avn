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

/* Aether-as-library entry points (ae/svnadmin/dump.ae). */
extern const char *aether_dump_prelude(int head, int rev_count, int rep_count);
extern const char *aether_rep_header(const char *sha, int size);
extern const char *aether_rev_pointer_block(int rev, const char *pointer_sha);
extern int         aether_algos_count(const char *spec);
extern const char *aether_algos_token(const char *spec, int i);
extern int         aether_line_starts_with_tag(const char *line, const char *prefix);
extern int         aether_parse_tagged_int(const char *line, const char *prefix, int fallback);
extern const char *aether_tagged_rest(const char *line, const char *prefix);
const char *svnae_fsfs_now_iso8601(void);

/* rep-cache.db helpers in ae/svnadmin/admin_db.ae (contrib.sqlite). */
extern const char *admin_db_init_schema(const char *repo);
extern const char *admin_db_list_reps_packed(const char *repo);
extern const char *aether_string_data(const void *s);
extern long        aether_string_length(const void *s);

/* ---- tiny helpers --------------------------------------------------- */

extern int aether_io_mkdir_p(const char *path);
extern int aether_io_write_atomic(const char *path, const char *data, int length);

/* slurp_small / parse_int previously lived here; unused after the
 * dump-body read moved to aether_repos_rev_blob_sha and the load
 * parser moved to ae/svnadmin/dump.ae::parse_tagged_int. */

/* ---- create --------------------------------------------------------- */

/* Golden list lives in ae/ffi/openssl/shim.c now; every binary that
 * links svnadmin also links the openssl wrapper, so we go through
 * svnae_openssl_hash_supported rather than keeping a duplicate. */
extern int svnae_openssl_hash_supported(const char *algo);
#define svnae_hash_supported svnae_openssl_hash_supported

/* Create the empty on-disk layout: dirs, format file, rep-cache schema.
 * No rev 0, no head. `algos_spec` is the comma-separated hash list
 * written into the format file (first entry = primary). Pass NULL or
 * "" to default to "sha1" for backward-compatible repos.
 * Shared by `create` (which goes on to seed rev 0) and `load` (which
 * replays revs from a dump). */
static int
create_bare(const char *repo, const char *algos_spec)
{
    if (aether_io_mkdir_p(repo) != 0) return -1;

    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/reps", repo);
    if (aether_io_mkdir_p(path) != 0) return -1;
    snprintf(path, sizeof path, "%s/revs", repo);
    if (aether_io_mkdir_p(path) != 0) return -1;

    /* Validate each algorithm against the golden list before writing. */
    const char *spec = (algos_spec && *algos_spec) ? algos_spec : "sha1";
    {
        int n = aether_algos_count(spec);
        for (int i = 0; i < n; i++) {
            if (!svnae_hash_supported(aether_algos_token(spec, i))) return -1;
        }
    }

    snprintf(path, sizeof path, "%s/format", repo);
    char fmt_line[128];
    int flen = snprintf(fmt_line, sizeof fmt_line, "svnae-fsfs-1 %s\n", spec);
    if (aether_io_write_atomic(path, fmt_line, flen) != 0) return -1;

    /* rep_cache + path_rev schema installed Aether-side via contrib.sqlite. */
    const char *err = admin_db_init_schema(repo);
    if (err && *err) return -1;
    return 0;
}

int
svnae_svnadmin_create(const char *repo)
{
    return svnae_svnadmin_create_with_algos(repo, NULL);
}

int
svnae_svnadmin_create_with_algos(const char *repo, const char *algos_spec)
{
    if (create_bare(repo, algos_spec) != 0) return -1;

    /* Phase 8.1: create the per-branch head layout alongside the
     * legacy $repo/head + $repo/revs/NNNNNN. Existing code paths
     * (commit finalise, server dispatch) still read the old layout
     * today; the new layout is written in parallel so branch-aware
     * code can come online incrementally.
     *
     * New layout:
     *   $repo/branches/main/head         ← one line: "rev=0"
     *   $repo/branches/main/revs/00/00/000000  ← rev pointer with fanout
     *   $repo/branches/main/spec          ← empty (include-all)
     *
     * The path_rev index table lives in rep-cache.db and is created
     * here so commit finalise can always INSERT without schema
     * concerns. */
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s/branches", repo);
    if (aether_io_mkdir_p(p) != 0) return -1;
    snprintf(p, sizeof p, "%s/branches/main", repo);
    if (aether_io_mkdir_p(p) != 0) return -1;
    snprintf(p, sizeof p, "%s/branches/main/revs", repo);
    if (aether_io_mkdir_p(p) != 0) return -1;
    snprintf(p, sizeof p, "%s/branches/main/revs/00/00", repo);
    if (aether_io_mkdir_p(p) != 0) return -1;
    /* Empty spec file — means "include everything from parent," but
     * for main (which has no parent) it means "full tree." */
    snprintf(p, sizeof p, "%s/branches/main/spec", repo);
    if (aether_io_write_atomic(p, "", 0) != 0) return -1;

    /* Path-rev table is installed by admin_db_init_schema above
     * (now part of the rep-cache.db schema rather than a follow-up
     * sqlite block here). */

    /* Seed rev 0: empty root dir + rev blob pointing at it. Use the
     * shared Aether rev-blob builder so the format stays in one place. */
    const char *empty_sha = svnae_rep_write_blob(repo, "", 0);
    if (!empty_sha) return -1;
    char empty_copy[65];
    size_t n = strlen(empty_sha);
    if (n >= sizeof empty_copy) return -1;
    memcpy(empty_copy, empty_sha, n + 1);

    extern const char *aether_rev_blob_body(const char *root, const char *branch,
                                            const char *props, const char *acl,
                                            int prev, const char *author,
                                            const char *date, const char *log);
    const char *rev0 = aether_rev_blob_body(empty_copy, "main", "", "", 0,
                                            "(init)", svnae_fsfs_now_iso8601(),
                                            "initial empty revision");
    const char *rev0_sha = svnae_rep_write_blob(repo, rev0, (int)strlen(rev0));
    if (!rev0_sha) return -1;

    char ptr_body[128];   /* sha256 = 64 hex + \n + NUL */
    int plen = snprintf(ptr_body, sizeof ptr_body, "%s\n", rev0_sha);
    char path[PATH_MAX];

    /* Legacy rev pointer — kept for Phase 8.1 so unmodified readers
     * continue to work. Phase 8.2+ will drop this once every reader
     * has moved to the per-branch layout. */
    snprintf(path, sizeof path, "%s/revs/000000", repo);
    if (aether_io_write_atomic(path, ptr_body, plen) != 0) return -1;

    /* New per-branch rev pointer. Two-level fanout: aa/bb/NNNNNN
     * where aa = rev/65536, bb = (rev/256)%256. At rev 0 that's
     * 00/00/000000. */
    snprintf(path, sizeof path, "%s/branches/main/revs/00/00/000000", repo);
    if (aether_io_write_atomic(path, ptr_body, plen) != 0) return -1;

    /* Legacy head file. */
    snprintf(path, sizeof path, "%s/head", repo);
    if (aether_io_write_atomic(path, "0\n", 2) != 0) return -1;

    /* New per-branch head file. Format: "rev=N\n". Richer than the
     * legacy one-int form — leaves room for tree-sha / spec-sha
     * caching in 8.2 without another format change. */
    snprintf(path, sizeof path, "%s/branches/main/head", repo);
    if (aether_io_write_atomic(path, "rev=0\n", 6) != 0) return -1;
    return 0;
}

/* ---- rep enumeration ------------------------------------------------ *
 *
 * admin_db_list_reps_packed (admin_db.ae) returns a "<N>\x02<sha>\x01
 * <size>\x02..." string; we slice it into svnae_rep_list for the
 * dump loop's index-based iteration. */

struct svnae_rep_list {
    char **sha1s;
    int   *sizes;
    int    n;
};

static struct svnae_rep_list *
list_reps(const char *repo)
{
    const char *packed = admin_db_list_reps_packed(repo);
    if (!packed) return NULL;
    const char *data = aether_string_data(packed);
    int total = (int)aether_string_length(packed);
    if (total <= 0) return NULL;

    /* Parse "<N>\x02..." — N is the row count. */
    const char *sep = memchr(data, '\x02', (size_t)total);
    if (!sep) return NULL;
    char nbuf[16];
    int nlen = (int)(sep - data);
    if (nlen <= 0 || nlen >= (int)sizeof nbuf) return NULL;
    memcpy(nbuf, data, (size_t)nlen);
    nbuf[nlen] = '\0';
    int n = atoi(nbuf);

    struct svnae_rep_list *L = calloc(1, sizeof *L);
    if (!L) return NULL;
    L->sha1s = (n > 0) ? calloc((size_t)n, sizeof *L->sha1s) : NULL;
    L->sizes = (n > 0) ? calloc((size_t)n, sizeof *L->sizes) : NULL;
    L->n = n;

    const char *p = sep + 1;
    const char *end = data + total;
    for (int i = 0; i < n && p < end; i++) {
        const char *eor = memchr(p, '\x02', (size_t)(end - p));
        if (!eor) eor = end;
        const char *mid = memchr(p, '\x01', (size_t)(eor - p));
        if (!mid) { L->n = i; break; }
        size_t shalen = (size_t)(mid - p);
        L->sha1s[i] = malloc(shalen + 1);
        memcpy(L->sha1s[i], p, shalen);
        L->sha1s[i][shalen] = '\0';
        char szbuf[16];
        size_t szlen = (size_t)(eor - (mid + 1));
        if (szlen >= sizeof szbuf) szlen = sizeof szbuf - 1;
        memcpy(szbuf, mid + 1, szlen);
        szbuf[szlen] = '\0';
        L->sizes[i] = atoi(szbuf);
        p = (eor < end) ? eor + 1 : end;
    }
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

    const char *prelude = aether_dump_prelude(head, head + 1, reps->n);
    if (write_all(out_fd, prelude, (int)strlen(prelude)) != 0) { free_rep_list(reps); return -1; }

    /* Each rep block. */
    for (int i = 0; i < reps->n; i++) {
        char *bytes = svnae_rep_read_blob(repo, reps->sha1s[i]);
        if (!bytes) { free_rep_list(reps); return -1; }

        const char *rep_hdr = aether_rep_header(reps->sha1s[i], reps->sizes[i]);
        if (write_all(out_fd, rep_hdr, (int)strlen(rep_hdr)) != 0) { svnae_rep_free(bytes); free_rep_list(reps); return -1; }
        if (write_all(out_fd, bytes, reps->sizes[i]) != 0) { svnae_rep_free(bytes); free_rep_list(reps); return -1; }
        if (write_all(out_fd, "\n", 1) != 0) { svnae_rep_free(bytes); free_rep_list(reps); return -1; }
        svnae_rep_free(bytes);
    }

    /* Rev pointers — trimmed read already handled by
     * ae/repos/rev_io.ae::repos_rev_blob_sha. */
    extern const char *aether_repos_rev_blob_sha(const char *repo, int rev);
    for (int r = 0; r <= head; r++) {
        const char *sha = aether_repos_rev_blob_sha(repo, r);
        if (!sha || !*sha) { free_rep_list(reps); return -1; }
        const char *block = aether_rev_pointer_block(r, sha);
        if (write_all(out_fd, block, (int)strlen(block)) != 0) { free_rep_list(reps); return -1; }
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
    /* Line-tag checks via ae/svnadmin/dump.ae helpers. */
    #define TAG_SENTINEL (-2147483647)
    len = read_line(in_fd, line, sizeof line);
    if (len < 0 || strcmp(line, "SVNAE-DUMP 1") != 0) return -1;
    len = read_line(in_fd, line, sizeof line);   /* FORMAT ... */
    if (len < 0 || !aether_line_starts_with_tag(line, "FORMAT")) return -1;
    len = read_line(in_fd, line, sizeof line);   /* HEAD ... */
    int head = aether_parse_tagged_int(line, "HEAD", TAG_SENTINEL);
    if (len < 0 || head == TAG_SENTINEL) return -1;
    len = read_line(in_fd, line, sizeof line);   /* REV-COUNT ... */
    int rev_count = aether_parse_tagged_int(line, "REV-COUNT", TAG_SENTINEL);
    if (len < 0 || rev_count == TAG_SENTINEL) return -1;
    len = read_line(in_fd, line, sizeof line);   /* REP-COUNT ... */
    int rep_count = aether_parse_tagged_int(line, "REP-COUNT", TAG_SENTINEL);
    if (len < 0 || rep_count == TAG_SENTINEL) return -1;

    /* Initialise target repo's bare layout (no seed rev 0 — the dump
     * will replay its own rev 0 below). Dumps currently always carry
     * sha1 blobs, so we create the target as sha1. If we later add
     * an ALGOS header to the dump format, pass it through here. */
    if (create_bare(repo, "sha1") != 0) return -1;

    /* REP-COUNT blocks. Hash may be sha1 (40 hex) or sha256 (64 hex);
     * the REP line gives us the full hex and the Aether helper returns
     * a pointer into the read buffer that we immediately strdup below. */
    for (int i = 0; i < rep_count; i++) {
        len = read_line(in_fd, line, sizeof line);
        if (len < 0) return -1;
        const char *sha_ref = aether_tagged_rest(line, "REP");
        if (!sha_ref || !*sha_ref) return -1;
        char sha[65];
        size_t slen = strlen(sha_ref);
        if (slen >= sizeof sha) return -1;
        memcpy(sha, sha_ref, slen + 1);

        len = read_line(in_fd, line, sizeof line);
        if (len < 0) return -1;
        int size = aether_parse_tagged_int(line, "SIZE", TAG_SENTINEL);
        if (size == TAG_SENTINEL) return -1;

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
        if (len < 0) return -1;
        int rev = aether_parse_tagged_int(line, "REV", TAG_SENTINEL);
        if (rev == TAG_SENTINEL) return -1;

        len = read_line(in_fd, line, sizeof line);
        if (len < 0) return -1;
        const char *sha = aether_tagged_rest(line, "POINTER");
        if (!sha || !*sha) return -1;

        char ptr_body[128];
        int plen = snprintf(ptr_body, sizeof ptr_body, "%s\n", sha);
        char ptr_path[PATH_MAX];
        snprintf(ptr_path, sizeof ptr_path, "%s/revs/%06d", repo, rev);
        if (aether_io_write_atomic(ptr_path, ptr_body, plen) != 0) return -1;
    }

    /* END + head. */
    len = read_line(in_fd, line, sizeof line);
    if (len < 0 || strcmp(line, "END") != 0) return -1;

    char head_body[32];
    int hlen = snprintf(head_body, sizeof head_body, "%d\n", head);
    char head_path[PATH_MAX];
    snprintf(head_path, sizeof head_path, "%s/head", repo);
    if (aether_io_write_atomic(head_path, head_body, hlen) != 0) return -1;

    return 0;
}
