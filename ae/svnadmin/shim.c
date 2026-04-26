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

/* svnae_svnadmin_create / _with_algos / the bare bootstrap helper all
 * moved to ae/svnadmin/create.ae in Round 77. The Aether-side
 * svnae_svnadmin_create_bare is exported for `svnadmin load` below. */
extern int svnae_svnadmin_create_bare(const char *repo, const char *algos_spec);

/* ---- rep enumeration ------------------------------------------------ *
 *
 * Round 79: per-record accessors over admin_db_list_reps_packed live
 * in admin_db.ae as admin_reps_count / _sha_at / _size_at; the dump
 * loop reads off the packed string directly. The C-side rep_list
 * struct + parser are gone. */
extern int         aether_admin_reps_count(const char *packed);
extern const char *aether_admin_reps_sha_at(const char *packed, int i);
extern int         aether_admin_reps_size_at(const char *packed, int i);

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

    /* The Aether helper returns an Aether-managed string (refcount-
     * stable through the dump loop). admin_reps_* accessors slice it
     * by index. */
    const char *reps_packed = admin_db_list_reps_packed(repo);
    if (!reps_packed) return -1;
    int n_reps = aether_admin_reps_count(reps_packed);

    const char *prelude = aether_dump_prelude(head, head + 1, n_reps);
    if (write_all(out_fd, prelude, (int)strlen(prelude)) != 0) return -1;

    /* Each rep block. */
    for (int i = 0; i < n_reps; i++) {
        const char *sha_borrowed = aether_admin_reps_sha_at(reps_packed, i);
        char sha[65];
        size_t sl = strlen(sha_borrowed);
        if (sl >= sizeof sha) return -1;
        memcpy(sha, sha_borrowed, sl + 1);
        int size = aether_admin_reps_size_at(reps_packed, i);

        char *bytes = svnae_rep_read_blob(repo, sha);
        if (!bytes) return -1;

        const char *rep_hdr = aether_rep_header(sha, size);
        if (write_all(out_fd, rep_hdr, (int)strlen(rep_hdr)) != 0) { svnae_rep_free(bytes); return -1; }
        if (write_all(out_fd, bytes, size) != 0) { svnae_rep_free(bytes); return -1; }
        if (write_all(out_fd, "\n", 1) != 0) { svnae_rep_free(bytes); return -1; }
        svnae_rep_free(bytes);
    }

    /* Rev pointers. */
    extern const char *aether_repos_rev_blob_sha(const char *repo, int rev);
    for (int r = 0; r <= head; r++) {
        const char *sha = aether_repos_rev_blob_sha(repo, r);
        if (!sha || !*sha) return -1;
        const char *block = aether_rev_pointer_block(r, sha);
        if (write_all(out_fd, block, (int)strlen(block)) != 0) return -1;
    }

    if (write_all(out_fd, "END\n", 4) != 0) return -1;
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
    if (svnae_svnadmin_create_bare(repo, "sha1") != 0) return -1;

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
