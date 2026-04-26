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

/* repos/shim.c — libsvn_repos query surface.
 *
 * Thin layer on top of fs_fs. Everything here reads the repo on-disk
 * format we built in Phase 3.x: $repo/head + $repo/revs/NNNNNN + rep-cache.db.
 *
 * Three public query operations for this phase:
 *   svnae_repos_log        — enumerate revisions with metadata
 *   svnae_repos_cat        — read file content at (rev, path)
 *   svnae_repos_list       — list entries of a directory at (rev, path)
 *   svnae_repos_info_rev   — metadata for a single revision
 *
 * The Aether side receives results via small opaque handles (already the
 * port's dominant idiom) and walks them with index-based accessors.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../subr/pin_list.h"
#include "../subr/packed_handle/packed_handle.h"

/* We piggyback on the rep-store shim for blob reads and on the fs_fs shim
 * for small-file reads. Declarations here; definitions link in from the
 * other shims (see aether.toml extra_sources for the test binary). */

char *svnae_rep_read_blob(const char *repo, const char *sha1_hex);

/* Dir-blob line parser ported to Aether (ae/fs_fs/dirblob.ae). */
extern int         aether_dir_count_entries(const char *body);
extern int         aether_dir_entry_kind(const char *body, int i);
extern const char *aether_dir_entry_name(const char *body, int i);
void  svnae_rep_free(char *p);

/* --- helpers --------------------------------------------------------- */

/* head_rev + rev_blob_sha ported to ae/repos/rev_io.ae. Direct calls
 * to aether_repos_head_rev at the remaining two call sites (blame
 * and the public svnae_repos_head_rev wrapper) — the static trampoline
 * was a pure rename. */
extern int aether_repos_head_rev(const char *repo);

/* ---- shared packed-string handle internals --------------------------
 *
 * Five accessor families (log / list / info / paths / blame) all
 * share the same shape: own one packed-string payload (parsed
 * Aether-side), remember the row count, and pin per-accessor
 * strdup'd copies so callers can hold pointers across calls. Same
 * pattern ra/shim.c uses (round 45) — independent here so each shim
 * can ship without a shared header.
 *
 * The packed-string parsers (aether_ra_log_*, aether_ra_paths_*,
 * etc.) live in ae/ra/packed.ae and are reused verbatim — the wire
 * record shape ae/repos/log.ae produces is identical to what
 * ae/ra/parse.ae produces, by design. */

/* The struct {packed, n, pins} + new/free moved to
 * ae/subr/packed_handle in round 56 (shared with ra/shim.c).
 * repos_handle_from_packed is now a one-line forward to
 * svnae_packed_handle_new — keep the local alias for readability
 * at the per-domain call sites. */
typedef int (*repos_count_fn)(const char *packed);

static struct svnae_packed_handle *
repos_handle_from_packed(const char *packed, repos_count_fn count)
{
    return svnae_packed_handle_new(packed, count);
}

/* --- log handle ------------------------------------------------------ */

extern const char *aether_repos_log_packed(const char *repo);
extern int         aether_ra_log_count(const char *packed);
extern int         aether_ra_log_rev(const char *packed, int i);
extern const char *aether_ra_log_author(const char *packed, int i);
extern const char *aether_ra_log_date(const char *packed, int i);
extern const char *aether_ra_log_msg(const char *packed, int i);

struct svnae_log *
svnae_repos_log(const char *repo)
{
    return (struct svnae_log *)repos_handle_from_packed(
        aether_repos_log_packed(repo), aether_ra_log_count);
}

int svnae_repos_log_count(const struct svnae_log *lg) { return svnae_packed_count(lg); }
int svnae_repos_log_rev   (const struct svnae_log *lg, int i) { return svnae_packed_int_at(lg, i, aether_ra_log_rev); }
const char *svnae_repos_log_author(struct svnae_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_author); }
const char *svnae_repos_log_date  (struct svnae_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_date); }
const char *svnae_repos_log_msg   (struct svnae_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_msg); }
void svnae_repos_log_free(struct svnae_log *lg) { svnae_packed_handle_free((struct svnae_packed_handle *)lg); }

/* --- cat / list: tree walk ---------------------------------------------
 *
 * Same directory blob format as Phase 3.3 (one line per entry,
 * "<kind> <sha1> <name>\n"). For cat we walk to the file entry and
 * read its blob. For list we read the directory blob and parse entries.
 */

extern const char *aether_repos_load_rev_blob_field(const char *repo, int rev,
                                                    const char *key);

static char *
root_dir_sha1_for_rev(const char *repo, int rev)
{
    const char *v = aether_repos_load_rev_blob_field(repo, rev, "root");
    if (!v || !*v) return NULL;
    return strdup(v);
}

/* resolve_path ported to Aether (ae/repos/resolve.ae). The split-
 * accessor shape (resolve_kind first, resolve_sha second) replaces
 * the pair of out-params. Both calls walk the path again, but at
 * these path lengths (typically 2-4 segments) that's negligible. */
extern int         aether_resolve_kind(const char *repo, const char *root_sha, const char *path);
extern const char *aether_resolve_sha (const char *repo, const char *root_sha, const char *path);

static int
resolve_path(const char *repo, const char *root_sha1, const char *path,
             char *out_kind, char **out_sha1)
{
    *out_kind = 0;
    *out_sha1 = NULL;
    int k = aether_resolve_kind(repo, root_sha1, path);
    if (k == 0) return 0;
    *out_kind = (char)k;
    const char *sha = aether_resolve_sha(repo, root_sha1, path);
    *out_sha1 = strdup(sha ? sha : "");
    return 1;
}

/* Read file content at (rev, path). Returns malloc'd bytes or NULL.
 * Caller frees with svnae_rep_free. */
char *
svnae_repos_cat(const char *repo, int rev, const char *path)
{
    char *root = root_dir_sha1_for_rev(repo, rev);
    if (!root) return NULL;
    char kind = 0;
    char *sha1 = NULL;
    int ok = resolve_path(repo, root, path, &kind, &sha1);
    free(root);
    if (!ok || kind != 'f') { free(sha1); return NULL; }
    char *data = svnae_rep_read_blob(repo, sha1);
    free(sha1);
    return data;
}

/* --- list handle ----------------------------------------------------- *
 *
 * svnae_repos_list(repo, rev, path) → handle of (name, kind) entries.
 * We deliberately don't surface the content sha1 — that's an fs_fs-
 * internal concern clients shouldn't know about.
 *
 * Round 54 folded this onto the shared svnae_repos_handle by adding
 * an Aether-side aether_repos_list_packed helper (ae/repos/list_packed.ae)
 * that emits the same packed shape ra_parse_list produces. The
 * ra_list_count / ra_list_name / ra_list_kind accessors from
 * ae/ra/packed.ae handle the walk verbatim. */

extern const char *aether_repos_list_packed(const char *repo, int rev, const char *path);
extern int         aether_ra_list_count(const char *packed);
extern const char *aether_ra_list_name (const char *packed, int i);
extern const char *aether_ra_list_kind (const char *packed, int i);

struct svnae_list *
svnae_repos_list(const char *repo, int rev, const char *path)
{
    return (struct svnae_list *)repos_handle_from_packed(
        aether_repos_list_packed(repo, rev, path),
        aether_ra_list_count);
}

int svnae_repos_list_count(const struct svnae_list *L) { return svnae_packed_count(L); }
const char *svnae_repos_list_name(const struct svnae_list *L, int i) { return svnae_packed_pin_at((void *)L, i, aether_ra_list_name); }
const char *svnae_repos_list_kind(const struct svnae_list *L, int i) { return svnae_packed_pin_at((void *)L, i, aether_ra_list_kind); }
void svnae_repos_list_free(struct svnae_list *L) { svnae_packed_handle_free((struct svnae_packed_handle *)L); }

/* --- info (single-revision metadata) --------------------------------- *
 *
 * One-shot lookup for a specific rev#. Aether's repos_info_packed does
 * the rev-blob read + field extraction in a single call; the C handle
 * just owns the packed "<rev>\x01<author>\x01<date>\x01<msg>" string.
 * Accessor shape matches the ra_info_* family, and we reuse those
 * accessors (ae/ra/packed.ae) verbatim. */

extern const char *aether_repos_info_packed(const char *repo, int rev);
extern int         aether_ra_info_rev(const char *packed);
extern const char *aether_ra_info_author(const char *packed);
extern const char *aether_ra_info_date(const char *packed);
extern const char *aether_ra_info_msg(const char *packed);

struct svnae_info *
svnae_repos_info_rev(const char *repo, int rev)
{
    /* info is a single record — count_fn is NULL so n stays 0. */
    return (struct svnae_info *)repos_handle_from_packed(
        aether_repos_info_packed(repo, rev), NULL);
}

int svnae_repos_info_rev_num(const struct svnae_info *I) { return svnae_packed_int_field(I, aether_ra_info_rev); }
const char *svnae_repos_info_author(struct svnae_info *I) { return svnae_packed_pin_field(I, aether_ra_info_author); }
const char *svnae_repos_info_date(struct svnae_info *I)   { return svnae_packed_pin_field(I, aether_ra_info_date); }
const char *svnae_repos_info_msg(struct svnae_info *I)    { return svnae_packed_pin_field(I, aether_ra_info_msg); }
void svnae_repos_info_free(struct svnae_info *I) { svnae_packed_handle_free((struct svnae_packed_handle *)I); }

int svnae_repos_head_rev(const char *repo) { return aether_repos_head_rev(repo); }

/* --- paths changed in a single revision -----------------------------
 *
 * For `svn log --verbose`: given rev N we want the list of paths added,
 * modified, or deleted in N relative to N-1. Both trees get flattened
 * into a sorted (path → (kind, sha1)) table by recursively walking
 * the dir blobs; then a linear merge classifies each path:
 *   present in both, same sha1 → skip
 *   present in both, different sha1 → 'M'
 *   only in N     → 'A'
 *   only in N-1   → 'D'
 *
 * Directories are included so that purely-empty dir adds/deletes show
 * up; their sha1 is whatever their dir-blob sha hashes to, which changes
 * whenever any descendant does, so subdirs that are structurally the
 * same across revs hash identically and get skipped. That's a happy
 * rep-sharing side effect. */

/* flat_tree + flatten_tree + flat_add + flat_free + flat_cmp all
 * ported to Aether (ae/repos/paths_changed.ae). */

/* Paths-changed diff ported to Aether (ae/repos/paths_changed.ae).
 * paths_changed_packed does the whole tree flatten + merge + diff +
 * pack in one call; we reuse the ra_paths_* accessors from
 * ae/ra/packed.ae since the "<N>\x02<action>\x01<path>\x02..." shape
 * is identical to what the RA client side serves up. */
extern const char *aether_paths_changed_packed(const char *repo, int rev);
extern int         aether_ra_paths_count(const char *packed);
extern const char *aether_ra_paths_action(const char *packed, int i);
extern const char *aether_ra_paths_path  (const char *packed, int i);

struct svnae_paths *
svnae_repos_paths_changed(const char *repo, int rev)
{
    if (rev < 0) return NULL;
    return (struct svnae_paths *)repos_handle_from_packed(
        aether_paths_changed_packed(repo, rev), aether_ra_paths_count);
}

int svnae_repos_paths_count(const struct svnae_paths *P) { return svnae_packed_count(P); }
const char *svnae_repos_paths_path(struct svnae_paths *P, int i)   { return svnae_packed_pin_at((void *)P, i, aether_ra_paths_path); }
const char *svnae_repos_paths_action(struct svnae_paths *P, int i) { return svnae_packed_pin_at((void *)P, i, aether_ra_paths_action); }
void svnae_repos_paths_free(struct svnae_paths *P) { svnae_packed_handle_free((struct svnae_packed_handle *)P); }

/* Public shim for server-side copy: resolve (rev, path) to its sha1 +
 * kind char. Returns 1 on success, 0 on miss. */
int
svnae_repos_resolve(const char *repo, int rev, const char *path,
                    char *out_sha1, char *out_kind)
{
    char *root = root_dir_sha1_for_rev(repo, rev);
    if (!root) return 0;
    char kind = 0;
    char *sha1 = NULL;
    int ok = resolve_path(repo, root, path, &kind, &sha1);
    free(root);
    if (!ok) { free(sha1); return 0; }
    size_t sl = strlen(sha1);
    /* out_sha1 must be big enough — callers now pass char[65]. */
    memcpy(out_sha1, sha1, sl + 1);
    *out_kind = kind;
    free(sha1);
    return 1;
}

/* Aether-callable split-accessor form. Returns 0 on miss instead of
 * an out-param pair. kind is 'f'(102), 'd'(100), or 0. */
int
svnae_repos_resolve_kind(const char *repo, int rev, const char *path)
{
    char sha[65]; char kind = 0;
    if (!svnae_repos_resolve(repo, rev, path, sha, &kind)) return 0;
    return (int)(unsigned char)kind;
}

const char *
svnae_repos_resolve_sha(const char *repo, int rev, const char *path)
{
    static __thread char buf[65];
    buf[0] = '\0';
    char kind = 0;
    svnae_repos_resolve(repo, rev, path, buf, &kind);
    return buf;
}

/* --- blame -----------------------------------------------------------
 *
 * Per-line attribution. The whole thing — paths_changed walk, LCS diff
 * against each prior version, annotation carry-forward — is ported to
 * ae/repos/log.ae::repos_blame_packed. That returns the same
 * "N\x02<rev>\x01<author>\x01<text>\x02..." shape ra_parse_blame
 * produces on the client side, so we reuse the ra_blame_*
 * accessors from ae/ra/packed.ae for the per-entry getters. */

extern const char *aether_repos_blame_packed(const char *repo, int target_rev, const char *path);
extern int         aether_ra_blame_count(const char *packed);
extern int         aether_ra_blame_rev(const char *packed, int i);
extern const char *aether_ra_blame_author(const char *packed, int i);
extern const char *aether_ra_blame_text(const char *packed, int i);

struct svnae_blame *
svnae_repos_blame(const char *repo, int target_rev, const char *path)
{
    return (struct svnae_blame *)repos_handle_from_packed(
        aether_repos_blame_packed(repo, target_rev, path),
        aether_ra_blame_count);
}

int svnae_blame_count(const struct svnae_blame *B) { return svnae_packed_count(B); }
int svnae_blame_rev(const struct svnae_blame *B, int i) { return svnae_packed_int_at(B, i, aether_ra_blame_rev); }
const char *svnae_blame_author(struct svnae_blame *B, int i) { return svnae_packed_pin_at((void *)B, i, aether_ra_blame_author); }
const char *svnae_blame_text(struct svnae_blame *B, int i)   { return svnae_packed_pin_at((void *)B, i, aether_ra_blame_text); }
void svnae_blame_free(struct svnae_blame *B) { svnae_packed_handle_free((struct svnae_packed_handle *)B); }
