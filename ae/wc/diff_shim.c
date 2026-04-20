/* ae/wc/diff_shim.c — svn diff.
 *
 * Produces a unified diff of each modified tracked file: pristine bytes
 * vs current disk bytes. For Phase 5.8 we shell out to /usr/bin/diff -u
 * with appropriate labels. Reference svn embeds its own libsvn_diff;
 * rewriting unified-diff formatting here is a distraction from the
 * working-copy port.
 *
 * API:
 *   svnae_wc_diff(wc_root, rel_path_or_empty)
 *     Walks the WC (or just the one path if non-empty) and for every
 *     tracked file in state=normal whose disk sha1 differs from its
 *     base_sha1, writes the unified diff to stdout.
 *     For state=added: shows as new file (diff vs empty "before").
 *     For state=deleted: diff vs empty "after".
 *     Returns 0 on success; negative on error.
 *
 * Output labels use the reference svn style:
 *   Index: <path>
 *   ===================================================================
 *   --- <path> (revision N)
 *   +++ <path> (working copy)
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <openssl/evp.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Externs from neighbouring shims. */
sqlite3 *svnae_wc_db_open(const char *wc_root);
void     svnae_wc_db_close(sqlite3 *db);

struct svnae_wc_nodelist;
struct svnae_wc_nodelist *svnae_wc_db_list_nodes(sqlite3 *db);
int         svnae_wc_nodelist_count(const struct svnae_wc_nodelist *L);
const char *svnae_wc_nodelist_path(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_kind(const struct svnae_wc_nodelist *L, int i);
const char *svnae_wc_nodelist_base_sha1(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_state(const struct svnae_wc_nodelist *L, int i);
int         svnae_wc_nodelist_base_rev(const struct svnae_wc_nodelist *L, int i);
void        svnae_wc_nodelist_free(struct svnae_wc_nodelist *L);

char *svnae_wc_pristine_get(const char *wc_root, const char *sha1);
int   svnae_wc_pristine_size(const char *wc_root, const char *sha1);
void  svnae_wc_pristine_free(char *p);

/* --- helpers --------------------------------------------------------- */

extern int svnae_wc_hash_file(const char *wc_root, const char *path, char *out);

static __thread const char *g_wc_root = NULL;

static int
sha1_of_file(const char *path, char out[65])
{
    if (!g_wc_root) return -1;
    return svnae_wc_hash_file(g_wc_root, path, out);
}

/* Write `data[0..len]` to a fresh tempfile; return malloc'd path. */
static char *
tmp_write(const char *data, int len)
{
    char *path = malloc(PATH_MAX);
    snprintf(path, PATH_MAX, "/tmp/svnae_diff_%d_%p.tmp", (int)getpid(), (void *)data);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(path); return NULL; }
    const char *p = data;
    int rem = len;
    while (rem > 0) {
        ssize_t w = write(fd, p, (size_t)rem);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(path); free(path); return NULL; }
        p += w; rem -= (int)w;
    }
    close(fd);
    return path;
}

/* Fork /usr/bin/diff -u --label "before" --label "after" BEFORE AFTER.
 * The child inherits our stdout, so output goes directly. */
static void
run_diff(const char *label_before, const char *label_after,
         const char *before, const char *after)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/diff", "diff", "-u",
              "--label", label_before, "--label", label_after,
              before, after, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    /* diff exits 0 if identical, 1 if differ, 2 on trouble; don't
     * propagate that — we always succeed as long as the diff tool ran. */
    (void)status;
}

/* Emit the svn-style header for a single path. */
static void
emit_header(const char *rel)
{
    printf("Index: %s\n", rel);
    printf("===================================================================\n");
}

/* --- public API ------------------------------------------------------ */

int
svnae_wc_diff(const char *wc_root, const char *rel_path_or_empty)
{
    g_wc_root = wc_root;
    sqlite3 *db = svnae_wc_db_open(wc_root);
    if (!db) return -1;

    struct svnae_wc_nodelist *L = svnae_wc_db_list_nodes(db);
    int n = svnae_wc_nodelist_count(L);

    for (int i = 0; i < n; i++) {
        const char *rel      = svnae_wc_nodelist_path(L, i);
        int         kind     = svnae_wc_nodelist_kind(L, i);
        int         state    = svnae_wc_nodelist_state(L, i);
        int         brev     = svnae_wc_nodelist_base_rev(L, i);
        const char *base_sha = svnae_wc_nodelist_base_sha1(L, i);

        if (rel_path_or_empty && *rel_path_or_empty
            && strcmp(rel, rel_path_or_empty) != 0)
            continue;

        if (kind != 0) continue;

        char disk[PATH_MAX];
        snprintf(disk, sizeof disk, "%s/%s", wc_root, rel);

        /* Determine the "before" and "after" buffers + labels. */
        char label_before[PATH_MAX + 64];
        char label_after [PATH_MAX + 64];
        char *before_path = NULL;
        char *after_path  = NULL;
        int need_cleanup_before = 0, need_cleanup_after = 0;

        if (state == 1 /*added*/) {
            /* Before = empty, after = disk. */
            before_path = tmp_write("", 0);
            after_path  = strdup(disk);
            need_cleanup_before = 1;
            snprintf(label_before, sizeof label_before, "%s\t(nonexistent)", rel);
            snprintf(label_after,  sizeof label_after,  "%s\t(working copy)", rel);
        } else if (state == 2 /*deleted*/) {
            /* Before = pristine, after = empty. */
            int psize = svnae_wc_pristine_size(wc_root, base_sha);
            char *p = svnae_wc_pristine_get(wc_root, base_sha);
            if (!p) continue;
            before_path = tmp_write(p, psize);
            after_path  = tmp_write("", 0);
            svnae_wc_pristine_free(p);
            need_cleanup_before = 1;
            need_cleanup_after  = 1;
            snprintf(label_before, sizeof label_before, "%s\t(revision %d)", rel, brev);
            snprintf(label_after,  sizeof label_after,  "%s\t(nonexistent)", rel);
        } else {
            /* state == normal: only diff if disk != pristine. */
            char disk_sha[65];
            if (sha1_of_file(disk, disk_sha) != 0) continue;
            if (strcmp(disk_sha, base_sha) == 0) continue;
            int psize = svnae_wc_pristine_size(wc_root, base_sha);
            char *p = svnae_wc_pristine_get(wc_root, base_sha);
            if (!p) continue;
            before_path = tmp_write(p, psize);
            after_path  = strdup(disk);
            svnae_wc_pristine_free(p);
            need_cleanup_before = 1;
            snprintf(label_before, sizeof label_before, "%s\t(revision %d)", rel, brev);
            snprintf(label_after,  sizeof label_after,  "%s\t(working copy)", rel);
        }

        if (!before_path || !after_path) {
            free(before_path); free(after_path);
            continue;
        }

        emit_header(rel);
        fflush(stdout);
        run_diff(label_before, label_after, before_path, after_path);

        if (need_cleanup_before) unlink(before_path);
        if (need_cleanup_after)  unlink(after_path);
        free(before_path);
        free(after_path);
    }

    svnae_wc_nodelist_free(L);
    svnae_wc_db_close(db);
    return 0;
}
