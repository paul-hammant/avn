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

/* Definitions link in from neighbouring shims (rep-store shim for blob
 * reads, fs_fs shim for small-file reads). */
char *svnae_rep_read_blob(const char *repo, const char *sha1_hex);
void  svnae_rep_free(char *p);

/* ---- shared packed-string handle internals --------------------------
 *
 * Five accessor families (log / list / info / paths / blame) share
 * the {packed, n, pins} struct from ae/subr/packed_handle (also used
 * by ra/shim.c). The packed-string parsers live in ae/ra/packed.ae —
 * record shape from ae/repos/log.ae matches ae/ra/parse.ae's by
 * design, so the same accessors decode both. */

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
    return (struct svnae_log *)svnae_packed_handle_new(
        aether_repos_log_packed(repo), aether_ra_log_count);
}

int svnae_repos_log_count(const struct svnae_log *lg) { return svnae_packed_count(lg); }
int svnae_repos_log_rev   (const struct svnae_log *lg, int i) { return svnae_packed_int_at(lg, i, aether_ra_log_rev); }
const char *svnae_repos_log_author(struct svnae_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_author); }
const char *svnae_repos_log_date  (struct svnae_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_date); }
const char *svnae_repos_log_msg   (struct svnae_log *lg, int i) { return svnae_packed_pin_at(lg, i, aether_ra_log_msg); }
void svnae_repos_log_free(struct svnae_log *lg) { svnae_packed_handle_free((struct svnae_packed_handle *)lg); }

/* --- cat ----------------------------------------------------------- */

extern const char *aether_repos_load_rev_blob_field(const char *repo, int rev,
                                                    const char *key);
extern int         aether_resolve_kind(const char *repo, const char *root_sha, const char *path);
extern const char *aether_resolve_sha (const char *repo, const char *root_sha, const char *path);

/* Read file content at (rev, path). Returns malloc'd bytes or NULL —
 * caller frees via svnae_rep_free. Stays on the C side because the
 * payload may contain embedded NULs and Aether's `string` ABI would
 * truncate at the first one on the FFI boundary. */
char *
svnae_repos_cat(const char *repo, int rev, const char *path)
{
    const char *root = aether_repos_load_rev_blob_field(repo, rev, "root");
    if (!root || !*root) return NULL;
    /* Take a copy now — the Aether helper hands back a pointer the
     * runtime is free to reuse on the next call. */
    char root_buf[65];
    size_t rlen = strlen(root);
    if (rlen >= sizeof root_buf) return NULL;
    memcpy(root_buf, root, rlen + 1);

    int k = aether_resolve_kind(repo, root_buf, path);
    if (k != 'f') return NULL;
    const char *sha = aether_resolve_sha(repo, root_buf, path);
    if (!sha || !*sha) return NULL;
    return svnae_rep_read_blob(repo, sha);
}

/* --- list handle ----------------------------------------------------- *
 *
 * svnae_repos_list(repo, rev, path) → handle of (name, kind) entries.
 * We deliberately don't surface the content sha1 — that's an fs_fs-
 * internal concern. Backed by ae/repos/list_packed.ae; ra_list_*
 * accessors decode the entries. */

extern const char *aether_repos_list_packed(const char *repo, int rev, const char *path);
extern int         aether_ra_list_count(const char *packed);
extern const char *aether_ra_list_name (const char *packed, int i);
extern const char *aether_ra_list_kind (const char *packed, int i);

struct svnae_list *
svnae_repos_list(const char *repo, int rev, const char *path)
{
    return (struct svnae_list *)svnae_packed_handle_new(
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
    return (struct svnae_info *)svnae_packed_handle_new(
        aether_repos_info_packed(repo, rev), NULL);
}

int svnae_repos_info_rev_num(const struct svnae_info *I) { return svnae_packed_int_field(I, aether_ra_info_rev); }
const char *svnae_repos_info_author(struct svnae_info *I) { return svnae_packed_pin_field(I, aether_ra_info_author); }
const char *svnae_repos_info_date(struct svnae_info *I)   { return svnae_packed_pin_field(I, aether_ra_info_date); }
const char *svnae_repos_info_msg(struct svnae_info *I)    { return svnae_packed_pin_field(I, aether_ra_info_msg); }
void svnae_repos_info_free(struct svnae_info *I) { svnae_packed_handle_free((struct svnae_packed_handle *)I); }

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

/* The full paths-changed diff (flatten + merge + pack) lives in
 * ae/repos/paths_changed.ae; we reuse the ra_paths_* accessors
 * since the wire shape matches. */
extern const char *aether_paths_changed_packed(const char *repo, int rev);
extern int         aether_ra_paths_count(const char *packed);
extern const char *aether_ra_paths_action(const char *packed, int i);
extern const char *aether_ra_paths_path  (const char *packed, int i);

struct svnae_paths *
svnae_repos_paths_changed(const char *repo, int rev)
{
    if (rev < 0) return NULL;
    return (struct svnae_paths *)svnae_packed_handle_new(
        aether_paths_changed_packed(repo, rev), aether_ra_paths_count);
}

int svnae_repos_paths_count(const struct svnae_paths *P) { return svnae_packed_count(P); }
const char *svnae_repos_paths_path(struct svnae_paths *P, int i)   { return svnae_packed_pin_at((void *)P, i, aether_ra_paths_path); }
const char *svnae_repos_paths_action(struct svnae_paths *P, int i) { return svnae_packed_pin_at((void *)P, i, aether_ra_paths_action); }
void svnae_repos_paths_free(struct svnae_paths *P) { svnae_packed_handle_free((struct svnae_packed_handle *)P); }

/* --- blame -----------------------------------------------------------
 *
 * Per-line attribution. paths_changed walk + LCS diff + annotation
 * carry-forward live in ae/repos/log.ae::repos_blame_packed. Same
 * record shape as ra_parse_blame; ra_blame_* accessors decode it. */

extern const char *aether_repos_blame_packed(const char *repo, int target_rev, const char *path);
extern int         aether_ra_blame_count(const char *packed);
extern int         aether_ra_blame_rev(const char *packed, int i);
extern const char *aether_ra_blame_author(const char *packed, int i);
extern const char *aether_ra_blame_text(const char *packed, int i);

struct svnae_blame *
svnae_repos_blame(const char *repo, int target_rev, const char *path)
{
    return (struct svnae_blame *)svnae_packed_handle_new(
        aether_repos_blame_packed(repo, target_rev, path),
        aether_ra_blame_count);
}

int svnae_blame_count(const struct svnae_blame *B) { return svnae_packed_count(B); }
int svnae_blame_rev(const struct svnae_blame *B, int i) { return svnae_packed_int_at(B, i, aether_ra_blame_rev); }
const char *svnae_blame_author(struct svnae_blame *B, int i) { return svnae_packed_pin_at((void *)B, i, aether_ra_blame_author); }
const char *svnae_blame_text(struct svnae_blame *B, int i)   { return svnae_packed_pin_at((void *)B, i, aether_ra_blame_text); }
void svnae_blame_free(struct svnae_blame *B) { svnae_packed_handle_free((struct svnae_packed_handle *)B); }
