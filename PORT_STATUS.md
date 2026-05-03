# svn-aether port — status

A clean-sheet reimplementation of Apache Subversion in the Aether
systems language. Not a mechanical C → Aether translation — we read
the svn C tree as a *reference for behaviour contracts* and wrote
our own on-disk formats, wire protocol, and algorithms. The reference
svn C code still lives at `subversion/libsvn_*` in this repo, read-only.

Plan document: `../svn-to-aether.md`
Aether bug/feature feedback: `AETHER_ISSUES.md`

## Headline

- **111 commits.** Each phase is its own commit, reviewable in isolation.
- **47 test suites**, ~507 assertions, all green.
- **C→Aether port, rounds 2 + 3** (30 leaf ports, 1 per commit): branch
  spec matcher, mergeinfo arithmetic, svn:ignore matcher, JSON
  escape/ints/records/responses, blobfield extractor, paths-index
  lookup + sort + ancestor-walk, props-blob → JSON, ACL rule evaluator
  + ancestor walker, /info specs emitter, dir-blob line parser
  (replacing 8 near-duplicate C scanners), $repo/format line parser,
  /info prelude, /rev/N/info body, /rev/N/hashes body, /rev/N/acl body,
  /list per-entry + redact-line builders, rev-blob body assembly,
  paths-index sort for acl/props blobs, svn cp ACL-auto-follow body
  scan, svnadmin dump-header builders, algos-spec comma split, full RA
  URL builder family. Each lives in an Aether source under
  `ae/*/<name>.ae`, compiled with `aetherc --emit=lib` and linked into
  its consumers. Hand-written C%: **82% → 69%** by end of round 5.
- **Rounds 4/5/6** (Aether 0.78.0/0.79.0): cjson was replaced with
  Aether's std.json in both `ra/shim.c` and `svnserver/shim.c` via a
  local compat layer; the `-lcjson` link-line dependency is gone
  entirely. FFI shims consolidated into `ae/ffi/{openssl,sqlite,
  zlib,utf8proc}/`. WC conflict-sidecar classifier, svn-status
  decision table, dir-blob find-by-name, dir-blob parse helpers,
  repo-path predicates (is_immediate_child / basename_after /
  covers), svnadmin dump-line parsers, and several hand-written
  JSON parsers in the verify path all ported. Issue #16's
  packed-int workaround unwound after discovering the real cause
  was `state` being a reserved keyword.  Hand-written C%: now
  **57%** of tracked source (generated `_generated.c` files are
  gitignored and not counted toward either side).
- **Round 7**: ported the svnserver /rev/N URL-tail parser
  (ae/svnserver/url_parse.ae), dropped four copies of hand-rolled
  `int_to_dec` + `digit_char` in favour of `std.string.from_int` now
  that it's available, and ported the WC pristine-store path builder
  to ae/working_copy/pristine_path.ae (handles the two-level XX/YY/ fanout).
- **Round 31** (current): **36.00% C, 64.00% Aether.**
  Round-30's rep-store port landed — the upstream Aether
  toolchain's `extra_sources` assembly buffer was bumped
  2 KiB → 8 KiB in a local patch (see AETHER_ISSUES.md #17,
  rebuilt at `~/scm/aether/build/ae`), which unblocks adding
  new `_generated.c` paths to svnserver's link line without
  triggering the silent truncation that sunk round 30.
  - New `ae/repo_storage/rep_store.ae` (175 LOC) owns the blob
    encode/decode half of the rep store: use_zlib decision
    with the 16-byte threshold, RAW/ZLIB-tagged envelope,
    binary-safe file write via fs.write_binary, inflate via
    zlib.inflate on read.
  - `aether_pristine_concat_binary` / `_slice_binary` moved
    from pristine_shim.c to rep_store_shim.c so both
    pristine_generated.c and rep_store_generated.c link one
    copy. The `aether_pristine_*` names kept for stability
    (now semi-misleading — worth a rename next time we touch
    every call site).
  - C side of rep_store_shim.c drops the direct `#include
    <zlib.h>` and the inline `compress2`/`uncompress` calls;
    the sqlite rep-cache + hash dispatch stay.
  - Binaries linking pristine-but-not-rep_store (svn,
    test_wc_pristine) also link rep_store_shim.c now so the
    hoisted concat/slice symbols resolve.
  - net: -17 LOC C, +175 LOC Aether. rep_store_shim.c
    stayed roughly the same size because the C drops were
    balanced by the two binary-safe helpers that had to move
    here. The structural win is format encapsulation —
    future rep-store format changes touch one Aether file.

- **Round 30**: no-LOC round. **36.28% C, 63.72% Aether**
  (unchanged).

  Attempted port of the fs_fs rep-store (sibling shape to round
  29's wc pristine port — same hash + zlib + binary-file-write
  pattern, using the now-ambient std.cryptography + std.zlib).
  The Aether side (`ae/repo_storage/rep_store.ae`, 200 LOC) worked
  end-to-end, but adding `ae/repo_storage/rep_store_generated.c` to
  the svnserver binary's `extra_sources` pushed the line over
  2 KiB, which silently truncated the gcc link command to
  `handler_copy_generat` and failed to link. Rolled back.

  Filed as AETHER_ISSUES.md #17 — the parse-side `extra_sources`
  buffer was bumped to 8 KiB in aether v0.85, but the
  assembly-side buffer (`char toml_extra[2048]` in
  `build_gcc_cmd`) stayed at 2 KiB. Until that's fixed, new
  `_generated.c` files on the svnserver link line cost more
  than a clean commit — same gotcha we inlined blob_build.ae
  around in round 23.

  The lesson is captured in migration_method.md's tooling
  section and feeds forward into round 31: once that buffer
  is raised upstream, the rep-store port's green path
  should drop in cleanly.

- **Round 29**: **36.28% C, 63.72% Aether.** Ported
  the wc pristine store (sha + zlib + binary file I/O) now that
  std.cryptography (0.88) and std.zlib (0.90+) landed upstream.
  - ae/working_copy/pristine.ae owns: wc_hash_bytes (dispatches
    sha1_hex/sha256_hex by the wc's configured algo),
    wc_hash_file (fs.read_binary + hash), wc_pristine_put
    (hash+dedup+zlib.deflate+fs.write_binary), wc_pristine_get
    (fs.read_binary+zlib.inflate), wc_pristine_has,
    wc_pristine_size.
  - C side keeps three small byte-level helpers
    (aether_pristine_pack_le32, _concat_binary, _slice_binary)
    that std.string can't do without a NUL-terminated
    intermediary. wc_hash_algo was moved to Aether in round 37
    (reads .svn/wc.db info via the subr.sqlite bindings used
    by fs_fs's rep-cache).
  - pristine_shim.c: 306 → 270 LOC. Dropped the direct zlib
    include, compress2 / uncompress calls, hex encoding, and
    the fs_try_read_binary TLS dance.
  - Bug in the first draft: AetherString has size_t length
    (8 bytes on 64-bit), not int32_t. My mis-sized struct
    read past length into data, handing concat a garbage
    byte count (`sdata=0x5`). Fix: match aether_string.h
    layout verbatim. Captured as a gotcha in
    migration_method.md.

- **Round 28**: **36.81% C, 63.19% Aether.** Ported
  svnae_repos_blame — the last big knot in repos/shim.c, ~245
  LOC of C doing line-level LCS, paths-changed walk, and
  per-line annotation carry-forward.
  - Aether side lives in ae/repos/log.ae::repos_blame_packed.
    Uses std.intarr for the O(na*nb) LCS dp table (2D flat
    indexing) and the per-line (offset, length) pairs (one
    packed intarr of size 2*n).
  - The annotation accumulator is itself the packed blame
    output shape — "<rev>\x01<author>\x01<text>\x02..." —
    same as ra_parse_blame emits on the client side. Each
    rev-step consumes the prev accumulator and produces a
    new one; no intermediate struct.
  - C collapses to the {packed, n, pin_list} handle pattern;
    accessors reuse aether_ra_blame_* from packed.ae.
  - repos/shim.c: 703 → 510 LOC (-193 LOC). split_lines,
    lcs_match, ann_push, struct line_ann, struct svnae_blame's
    old items array, and the 90-LOC walk loop all gone from C.

- **Round 29** (current): **36.81% C, 63.19% Aether (unchanged).**
  Code-quality consolidation: the `pin_list` / `pin_str` /
  `pin_list_free` pattern for per-handle string memory management
  was identically implemented in both ra/shim.c (~28 LOC) and
  repos/shim.c (~28 LOC). Moved to a shared ae/subr/pin_list.h
  included by both, eliminating ~56 LOC of duplication across the
  shims. No functional change, no port activity — just cleanup
  toward the natural end state at 36-37% C.

- **Round 30** (current): **36.66% C, 63.34% Aether.** Inlined
  trivial `mkdir_p` and `write_file_atomic` wrappers in
  svnadmin/shim.c. Both are pure pass-throughs to aether_io_mkdir_p
  and aether_io_write_atomic (7-11 call sites each); fall in the
  "always inline" range per pattern 3. svnadmin/shim.c: 511 → 504
  LOC (-7 LOC). Total C drop: -7 LOC. Final steady state: 36.66% C.

- **Rounds 26-27**: **37.85% C, 62.15% Aether.** Leaf
  sweep after the structural passes:
  - svnae_repos_info_rev: ported to ae/repos/log.ae's
    repos_info_packed. One rev-blob read in Aether → packed
    "<rev>\x01<author>\x01<date>\x01<msg>" record → C reuses
    ra_info_* accessors from packed.ae verbatim. 4× fewer
    rev-blob reads per info lookup at runtime.
  - Nine trivial C→aether_* trampolines inlined at call sites:
    repos head_rev (2), svnserver acl_allows_mode / acl_allows /
    based_on_check / spec_allows / request_branch / header_or_null,
    wc/update + wc/merge walk_remote, fs_fs/rep_store mkdir_p.
    All were pure name-translation layers left over from when
    the ports landed incrementally.
  - mkdir_p + write_atomic alias pairs inlined in wc/checkout,
    wc/pristine, wc/revert, fs_fs/rep_store. Kept the ones in
    commit_shim.c (12 uses) and svnadmin/shim.c (17 uses) where
    the short alias still earns its line.

- **Round 25**: **38.08% C, 61.92% Aether** (C now
  under 10,000 LOC for the first time). A particularly silly
  round-trip: Aether built `globs_joined` as a newline-separated
  string → called svnserver_branch_create_globs (C) → C split
  it back into a `char **globs` array → called svnae_branch_create
  → svnae_branch_create rejoined the array into `globs_joined`
  → passed to aether_filter_dir.
  - Changed svnae_branch_create's signature to take
    globs_joined directly.
  - Dropped svnserver_branch_create_globs entirely.
  - branch_create_parse.ae calls svnae_branch_create directly
    (its extern no longer has to route through the svnserver
    trampoline).

- **Round 24**: **38.12% C, 61.88% Aether.** Leaves
  round picking up the last two packed-string reparsers in
  ra/shim.c that rounds 21-22 didn't cover.
  - `svnae_ra_get_props` — same round-trip as log/paths/blame/
    list: Aether produces "<N>\x02<name>\x01<value>\x02...", C
    rebuilt a struct-of-arrays with per-field accessors.
    Collapsed to the {packed, n, pin_list} pattern + new
    ra_props_count/name/value accessors in ae/client/packed.ae.
  - `verify_dir` inside the verify client had an inline copy
    of the same reparse (~20 LOC) to populate its local
    `struct entry[]`. Swapped it for ra_list_count/name/kind
    calls over a strdup'd packed copy (the recursion hits
    many Aether string calls, so we own the packed buffer
    locally).

- **Round 23**: **38.20% C, 61.80% Aether.** Cut the
  commit-blob-build knot — four `svnserver_build_*_joined` C
  wrappers (svnserver/shim.c, ~85 LOC) that split "\n"-joined
  parallel strings back into arrays, plus four `svnae_build_*_blob`
  C functions (fs_fs/commit_shim.c, ~100 LOC) that concatenated
  those arrays into the final blob body with sort+write. The
  whole round trip was pure text work bouncing across the FFI.
  - Aether (commit_parse.ae) had already been accumulating the
    keys / values / paths / shas / rules as `\n`-joined strings
    — now it builds the "key=value\n" / "+user\n" /
    "<sha> <path>\n" bodies directly and calls
    svnae_rep_write_blob itself. The sort-by-key for props
    uses a selection-sort over a packed `<f0>\x01<f1>\x02...`
    stream; paths-index reuses aether_paths_index_sort_by_path
    which was already ported. C keeps only the rep-store write
    (which it had all along — the svnae_build_* fronts were
    just allocation + snprintf).
  - svnserver/shim.c: 824 → 711 LOC (−113). fs_fs/commit_shim.c:
    669 → 567 LOC (−102). Total C drop: −215 LOC.
  - Builder helpers live inline in commit_parse.ae rather than
    a separate module because a new `_generated.c` path on
    aether.toml's extra_sources pushed the gcc link command
    past an argv-length limit in the aether build tool and
    truncated `handler_commit_generated.c` to `han`. Single-
    file inline avoids the extra path.

- **Round 22**: **39.01% C, 60.99% Aether.** Same
  knot cut again, this time on the server-side twin of round 21:
  `svnae_repos_log` and `svnae_repos_paths_changed` both followed
  the same "eager build into struct-of-arrays, expose indexed
  accessors" pattern — but worse than the RA client, because
  here the data is local and the builder was reading HEAD+1
  rev blobs in C with its own strdup/free plumbing.
  - New `ae/repos/log.ae::repos_log_packed(repo)` does the whole
    rev walk in Aether, emitting the exact same "<N>\x02<rev>
    \x01<author>\x01<date>\x01<msg>\x02..." shape ra_parse_log
    does. The C accessors reuse ra_log_* from ae/client/packed.ae
    directly — same record shape, zero new walkers.
  - `ae/repos/paths_changed.ae` gained `paths_changed_packed`
    + `pack_amd_pairs` to produce the packed form from the
    existing "<action>\n<path>\n" merge output; C accessors
    reuse ra_paths_*.
  - repos/shim.c: 757 → 704 LOC. Dropped parse_field,
    rev_blob_sha1, the svnae_log/svnae_paths structs-of-arrays,
    their five log + three paths accessors and the associated
    free machinery. Added the same {packed, n, pin_list}
    handle + pin_str helper pattern round 21 established for
    ra/shim.c.
  - Packed-accessor reuse: ae/client/packed_generated.c is now linked
    by svnadmin, svnserver, and test_repos in addition to the
    existing RA consumers. One set of walkers serves both
    sides of the wire.

- **Round 21**: **39.36% C, 60.64% Aether** — crossed
  below 40% C. Untied the biggest knot in `ra/shim.c`: four
  near-identical ~40-line packed-string reparsers (log, paths,
  blame, list) plus a 28-line single-record parser (info) all
  disappeared in favour of typed accessors over the packed
  string itself.
  - New `ae/client/packed.ae` (366 LOC Aether) exposes
    `ra_{log,paths,blame,list}_{count,rev,author,...}` and the
    five `ra_info_*` accessors. Each one walks the packed
    `<N>\x02<entry>\x02<entry>...` string on demand.
  - C side keeps only `{char *packed; int n; struct pin_list pins;}`
    per handle. No more struct-of-arrays, no more per-field
    tokenising loops. Accessors `strdup` each Aether-returned
    string into a per-handle pin list so the stable-pointer
    contract the C API always had still holds (a per-accessor
    TLS slot was tried first and fell over on recursive checkout
    where `prefix` and `name` aliased the same slot across a
    recursive call).
  - ra/shim.c: 1507 → 1445 LOC (−62), but the old code was
    tangled tokenising that's now much cleaner structurally.
  - All 32 shell tests green.

- **Round 20**: **40.07% C, 59.93% Aether.** Second
  cross-shim pass, this time on `svnae_*` forward decls. The wc
  shims had accumulated large decl blocks for symbols whose call
  sites had all migrated into Aether siblings (update_apply.ae,
  merge3 etc.); the bodies live in other translation units, so
  the forward decls were pure noise once no local caller
  remained.
  - wc/update_shim.c: dropped ~45 lines of forward decls —
    svnae_wc_db_upsert_node/delete_node/set_conflicted,
    svnae_merge3_apply, svnae_wc_pristine_get/size/free/put,
    the per-item nodelist accessors (kind/base_rev/base_sha1/
    state/path), svnae_ra_list family, svnae_ra_cat/free,
    svnae_ra_get_props + props accessors, svnae_wc_propset/
    propdel/proplist family. Kept only what the shim itself
    still calls: db_open/close/set_info/get_info/list_nodes/
    nodelist_count/nodelist_free, wc_info_free, ra_head_rev.
  - wc/merge_shim.c: dropped ~26 lines analogously — get_node
    family, ra_list family, ra_cat, wc_pristine_put,
    wc_db_delete_node/set_conflicted, wc_hash_file,
    svnae_merge3_apply.
  - svnserver/shim.c: dropped svnae_rep_read_blob,
    svnae_rep_free, all forward decls — no remaining caller
    in this translation unit.
  - fs_fs/commit_shim.c: same two dropped.

- **Round 19**: **40.23% C, 59.77% Aether.** Another
  dead-decl sweep across the remaining shims after the svnserver
  cleanup in round 18:
  - svnserver/shim.c: dropped aether_blobfield_get (unused),
    aether_handler_{branch_create,commit,copy} extern decls —
    the std.http dispatcher binds those Aether handlers directly
    now, so the C-side forward decls were leftover scaffolding.
  - fs_fs/commit_shim.c: dropped aether_blobfield_get and
    aether_spec_matches_any (both had no remaining C callers; the
    in-C rev-blob field probe moved to aether_repos_load_rev_blob_field
    last round, and branch-spec matching goes through
    aether_branch_spec_allows).
  - fs_fs/rep_store_shim.c: dropped aether_format_{primary_hash,
    secondary_count,secondary_hash} — superseded by rev_io.ae's
    repo_primary_hash / repo_secondary_hashes_joined which do
    the file-read + parse together.
  - repos/shim.c: dropped aether_dir_entry_sha (only kind + name
    are pulled per dir entry; the sha accessor was an unused
    leftover from the dir-blob parser port).

- **Round 18**: **40.33% C, 59.66% Aether.** Cleanup
  round focused on pulling one more layer of C-side trampolines
  up and pruning stale forward decls:
  - svnae_repo_secondary_hashes parse → ae/repos/rev_io.ae
    (\n-joined names; C splits into char[4][32]).
  - svnadmin dump rev-pointer read → aether_repos_rev_blob_sha.
  - repos_info_rev + root_dir_sha1_for_rev both use the shared
    aether_repos_load_rev_blob_field helper now.
  - count_reps_recurse → ae/repo_storage/count_reps.ae.
  - Dead trampolines gone from update_shim.c (ingest_props),
    svnserver/shim.c (acl_user_has_rw_subtree, 24 stale aether_*
    extern decls, 5 obsolete svnae_txn_*/commit_finalise forward
    decls), svnadmin/shim.c (slurp_small).
  - Dead read_format_line in rep_store_shim.c.

  svnserver/shim.c: 918 → 868 LOC.

- **Round 17**: **40.75% C, 59.24% Aether.** Finishing
  off the cJSON cleanup and picking up a few small fs_fs helpers:
  - Dead cJSON compat layers removed from ra/shim.c and
    svnserver/shim.c (both had no remaining users; kept as
    historical scaffolding from pre-Aether-stdlib-json days).
  - ra_parse_rev_response collapses three inline four-line
    cJSON {"rev":N} parsers into one Aether helper.
  - svnae_env_get → std.os::getenv.
  - svnae_branch_spec_allows → ae/repo_storage/branch_spec.ae.
  - load_rev_root_sha1 + rev_blob_field both swap to the existing
    ae/repos/rev_io.ae two-hop reader (closes the round-14
    deferred port).
  - svnae_repo_primary_hash → ae/repos/rev_io.ae.
  - Plus the 5.5 MB subversion/bindings/ reference tree deleted.

- **Round 16**: **41.28% C, 58.71% Aether.** The 8 MB
  vendored `apr/` tree is deleted (zero Aether references). The
  two remaining svnserver C-side dispatchers move up:
  - /rev/N/<sub> dispatcher: 140 LOC → ae/svnserver/handle_repo_rev.ae.
  - Top-level /repos/{r}/<...> dispatcher: 55 LOC →
    ae/svnserver/dispatch.ae (std.http's route callback now points
    directly at aether_svnserver_dispatch).
  - parse_field's "present but empty" rescan: dropped (no caller
    differentiates from absent).
  - More dead C — handle_repo_info/log/rev/path_* trampolines,
    parse_repo_and_tail, parse_rev_from_tail, compute_redacted_dir_sha,
    rtree_find / rt_find lookups: -110 LOC.
  - req_header delegated to std.http's http_get_header (same
    strcasecmp loop).
  - respond_* statics merged into Aether-callable wrappers; no
    intermediate indirection.
  - Three "from_body" trampolines (branch_create, copy, commit)
    dropped — handlers bind to aether_* exports directly.

  svnserver/shim.c: 1015 → 936 LOC; total C share crosses below
  42 %.

- **Round 15**: **42.70% C, 57.29% Aether.** Smaller-
  bore cleanup round focusing on the last cJSON sites and some
  dead-code pruning:
  - `verify_secondaries_in_dir`: its two inline cJSON walkers
    (list and /hashes secondaries) switch to the already-ported
    `ra_parse_list` and a new `ra_parse_hashes_secondaries`.
  - `ingest_props` (svn update's per-path prop reconciliation)
    → ae/working_copy/update_props.ae. Opaque-handle walk via already-
    Aether-callable ra/wc accessors.
  - `read_format_line` ($repo/format reader) →
    ae/repos/rev_io.ae::repos_format_line.
  - `based_on_check` (PUT/DELETE optimistic-concurrency) →
    ae/svnserver/based_on_check.ae.
  - Dead code sweep: `sb_*` JSON builder scaffolding in
    svnserver/shim.c and read_small / trim_trailing_newline /
    parse_int / rev_pointer_path in repos/shim.c.

- **Round 14**: **43.40% C, 56.59% Aether.** Focus
  shifts to the RA client side + repos helpers:
  - `svnae_ra_commit_finish`: 140 LOC of cJSON-heavy body
    serialisation moves up to ae/client/commit_build.ae. std.json
    for construction; C-side accessors expose struct svnae_ra_commit.
  - `svnae_ra_server_copy` + `svnae_ra_branch_create` bodies:
    small cJSON builders, also Aether via std.json.
  - verify_dir: swapped inline cJSON walker for the already-ported
    ra_parse_list packed-string parser.
  - `head_rev` + `rev_blob_sha1` → ae/repos/rev_io.ae.
  - `walk_remote` (svn update) → ae/working_copy/update_walk.ae.
  - `walk_remote` (svn merge) → ae/working_copy/merge_walk.ae.

  One attempted port deferred: fs_fs/commit_shim.c::load_rev_root_sha1
  via the new repos_load_rev_blob_field helper produced a runtime
  crash during /commit — set aside; the function sits in
  rev_io.ae unused until diagnosed.

- **Round 13**: **44.44% C, 55.55% Aether.** With
  std.json reachable from Aether, the three remaining
  cJSON-heavy server-side mutation handlers all move up:
  - `svnserver_branch_create_from_body` (45 LOC C → 80 LOC
    ae/svnserver/branch_create_parse.ae). Includes the include-
    globs array, passed to C as a newline-joined string.
  - `svnserver_copy_from_body` (60 LOC C → 130 LOC
    ae/svnserver/copy_parse.ae). Whole-body parse + auth + ACL +
    spec + resolve + auto-follow + commit all Aether-side.
  - `svnserver_commit_from_body` (220 LOC C → 280 LOC
    ae/svnserver/commit_parse.ae). Biggest JSON-walker in the
    port: multi-edit txn + per-path props map + per-path ACL
    map. Four new "joined" C wrappers split newline-joined
    strings back into char** arrays for the blob builders that
    don't have an Aether-friendly signature.
  - RA client (ae/client/shim.c) also got parse-side ports for
    head_rev, hash_algo, info_rev, log, paths_changed, props,
    blame, and list. C still owns curl + auth headers + struct
    allocation; std.json does the JSON walk.
  One bug along the way worth noting: auto_follow_copy_acl
  returned via a TLS-thread-local buffer that subsequent
  txn_copy path also used, silently trashing the ACL sha before
  commit_finalise ran. Fixed with an explicit string.copy
  snapshot, matching the existing pattern in the other 49 sites.

  svnserver/shim.c: 1623 → 1390 LOC over rounds 12+13.

- **Round 12**: **46.20% C, 53.79% Aether.** Aether 0.83/0.84
  landed three round-3 wish items: `std.intarr` (0.83),
  `string.strip_prefix` + `string.copy` (0.84), and confirmed
  `export extern` already works at module-import time. Used the
  new stdlib helpers to retire ~60 LOC of hand-written URL
  slicing across 8 handlers, and gave the TLS-snapshot idiom
  (44 call sites, 12 files) a discoverable name.

  Four structural internals then ported on top of the cleanup:
  - `acl_user_has_rw_subtree` (28 LOC C → ae/svnserver/acl_subtree.ae):
    the /copy handler's "you can only copy what you fully own at
    RO + RW" recursive walker.
  - `load_rev_blob_field` (22 LOC C → ae/svnserver/rev_load.ae):
    the two-hop "read $repo/revs/NNNNNN → read rep blob → pull
    field" utility used by every rev-scoped ACL/props/root read.
  - `auto_follow_copy_acl` (21 LOC C → added to
    ae/svnserver/copy_acl.ae): end-to-end ACL auto-follow for
    svn cp — now composes copy_acl_follow alongside its users.
  - `acl_allows_mode` (35 LOC C → ae/svnserver/acl_mode.ae): the
    ancestry-walking mode check. Retires the last hand-written
    newline-string tokeniser in svnserver/shim.c.

- **Round 11**: **46.82% C, 53.17% Aether.** Started the session
  at 68% C. Aether 0.81.0 made std.http request/response
  accessors tolerate externally-constructed `HttpRequest*` /
  `HttpServerResponse*` pointers, so C-dispatched handlers can
  flow all the way through Aether. All svnserver read+mutate
  handlers Aether-owned:

  - Read: `handler_info`, `handler_log`, `handler_rev_info`,
    `handler_rev_cat`, `handler_rev_list`, `handler_rev_paths`,
    `handler_rev_hashes`, `handler_rev_acl`, `handler_rev_props`,
    `handler_rev_blame`, `handler_path_get`
  - Mutate: `handler_path_put`, `handler_path_delete`,
    `handler_branch_create`, `handler_copy`, `handler_commit`

  Each C branch is now a 3-line `aether_*_handle(req, res)`
  extern call. For mutating handlers where cJSON + dynamic-sized
  string arrays are central (branch_create, copy, commit), the
  JSON parse + mutation stays in C behind a typed wrapper that
  returns -1..-N for distinct failure kinds; the Aether handler
  maps each to the right HTTP response. Only the dispatcher
  itself, auth_context, and a few static helpers remain on the
  C side of the handler layer. Aether 0.82.0's `fs.read_binary`
  NUL-preservation fix retired the TLS-buffer escape hatch carried
  in three places. `std.intarr` landed but we haven't used it yet
  (blame LCS would).

- **Round 10**: svnserver JSON body builders move to Aether.
  Each handler's body-building logic lives in a dedicated
  `ae/svnserver/<route>_json.ae` module. The C side is now pure
  routing + ACL-check + extern call + respond. Ported: /info
  (info_json.ae), /log + /rev/N/paths + /rev/N/blame + /rev/N/hashes
  (all in log_json.ae), /rev/N/list with the body+redact-blob pair
  (list_json.ae), /rev/N/acl (acl_resolve.ae), /rev/N/props
  (same file, props_resolve). The packed-return trick
  ("<sha>\x01<json>") shuttles two values through one FFI call
  when Merkle headers need the sha separately. C% from 52% → 50%.

- **Round 9**: the Gordian knots fall. Three structural
  recursive walkers ported in a single session after `--emit=lib
  --with=fs` landed:
  - `filter_dir_recursive` (fs_fs, 89 lines of C → ae/repo_storage/filter.ae)
  - `rebuild_dir_c` (fs_fs, 292 lines of C → ae/repo_storage/rebuild.ae)
    — the core commit tree rebuilder that had been flagged as
    "weeks of work" earlier in the session
  - `compute_redacted_dir_sha` (svnserver, 50 lines → ae/svnserver/redact.ae)
  Plus two more algorithmic ports:
  - `svnae_wc_update` apply pipeline (wc, 135 lines → ae/working_copy/update_apply.ae)
  - `svnae_wc_merge` apply pipeline (wc, 135 lines → ae/working_copy/merge_apply.ae)
  - `svnae_repos_paths_changed` flatten+diff (repos, 65 lines →
    ae/repos/paths_changed.ae)
  - `resolve_path` (repos, 70 lines → ae/repos/resolve.ae)
  Pattern: every recursive walker reads/writes blobs through C
  externs, represents its in-memory accumulator as a newline-
  separated string in the same `K SHA NAME\n` format the dir-blob
  parsers already understand. Hand-written C% now at **52%**, from
  57% at round-8 close.

- **Round 8**: Aether shipped `--emit=lib --with=fs` after
  a feedback pass from this port (`~/scm/aether/stdlib_wish.md`), so
  std.fs is now reachable from our `.ae` files. `ae/subr/io.ae`
  exports atomic-write / mkdir-p / slurp / file_size / is_regular
  / listdir / stat_kind / exists / unlink / rmdir / rename over
  `std.fs` and `std.dir`. **All direct I/O syscalls are gone from
  the shims.** A grep for `open|fopen|stat|lstat|fstat|mkdir|unlink|
  rmdir|opendir|readdir|rename|fsync` across `ae/*/shim.c` +
  `ae/*/*_shim.c` now finds zero hits; every hand-rolled
  write-atomic, mkdir-p, slurp, dir-walk, copy-file, and
  existence-check has been folded through the Aether io layer.
  rep_store_shim and pristine_shim use the TLS-buffered
  `fs_try_read_binary` for binary-safe blob reads (the string
  wrapper `fs.read_binary` copies via string_concat which would
  truncate at embedded NULs).
  Hand-written C%: now **~57%** of tracked source (from 68% at
  round-7 baseline).
  Mix of in-language `.ae` tests and end-to-end shell harnesses that
  spin up a real HTTP server and drive it with curl and the built
  `svn` CLI.
- **~14,400 lines** total (Aether + C shims + shell test drivers).
- Daily-driver workflow works end to end: checkout → edit → status
  → diff → add → rm → commit → update → branch → merge.

## Binaries we ship

| Binary | What it does |
|---|---|
| `svn` | Client CLI: `checkout`, `status`, `add`, `rm`, `cp`, `mv`, `commit`, `update`, `revert`, `diff`, `log`, `cat`, `ls`, `info`, `propset`/`get`/`del`/`list`, `merge` |
| `aether-svnserver` | Read/write HTTP server. JSON for metadata, base64 for bytes over JSON, raw bytes for cat. |
| `svnadmin` | Repository admin: `create`, `dump`, `load` (our own portable dump format; not reference-svn compatible by design). |
| `svnae-seed` | Test-only helper that populates a repo with a known tree across three commits. |

## Running the stack

```bash
ae=/home/paul/scm/aether/build/ae

# Build what you need, or run the full test sweep:
for f in ae/svn/main.ae ae/svnserver/main.ae ae/svnserver/seed.ae \
         ae/svnadmin/main.ae; do
    "$ae" build "$f" -o "/tmp/$(basename "${f%.ae}")"
done

# Or from the project root:
/tmp/svnadmin create /srv/repo
/tmp/main demo /srv/repo 8080 &     # that's aether-svnserver
/tmp/main checkout http://localhost:8080/demo wc
```

Run all 30 test suites: a bash one-liner at the end of this doc.

## Phase map

Named after the plan's Phase N. Plan: `../svn-to-aether.md`.

| Phase | Title | Status | Commit |
|------:|:------|:-------|:-------|
| 0.1 | FFI smoke tests (sqlite, zlib, utf8proc, openssl) | ✅ | `d39e669` |
| 0.2 | Error chain with file/line | ✅ | `ba4a69c` |
| 0.3 | Path handling (relpath + dirent) | ✅ | `da5c0a6` |
| 0.4 | Checksums + zlib + atomic I/O | ✅ | `f729696`, `dd22308` |
| 0.5 | SQLite binding | ✅ | `1de5530` |
| 1-lite | utf8proc NFC binding | ✅ | `1be7911` |
| 2 | svndiff1 encode/decode + xdelta round-trip | ✅ | `6b8f0d1`, `879f576` |
| 3.1 | FSFS content-addressable blob store | ✅ | `a9648ee` |
| 3.2 | Revisions + rep-sharing across revisions | ✅ | `f1aebfe` |
| 3.3 | Nested dirs + txn API | ✅ | `df9d77c` |
| 4 | libsvn_repos query API (log, cat, list, info) | ✅ | `9d032af` |
| 6 | aether-svnserver (read-only HTTP) | ✅ | `c6cef15` |
| 6.5 | POST /commit (read/write loop closed) | ✅ | `dfd6567` |
| 7 | libsvn_ra (Aether-callable REST client) | ✅ | `b411afa` |
| 10 (stateless subset) | svn CLI | ✅ | `f90f31d` |
| 5.1 | wc.db metadata store | ✅ | `f882fb6` |
| 5.2 | Pristine store | ✅ | `bb865f1` |
| 5.3 | Checkout | ✅ | `69ec5bc` |
| 5.4 | Status | ✅ | `f009280` |
| 5.5 | add/rm | ✅ | `b1f7c79` |
| 5.6 | WC-backed commit | ✅ | `211ba67` |
| 5.7 | svn update | ✅ | `5aa005e` |
| 5.8 | svn revert + svn diff | ✅ | `1ffafb8` |
| 5.9 | svn cp / mv (WC-local) | ✅ | `b6f2741` |
| 5.10 | WC-side properties | ✅ | `2c37fc4` |
| 5.11 | svn:ignore filter for status | ✅ | `6202fb5` |
| 5.12a | Branches (server-side copy) | ✅ | `94c06f0` |
| 5.12b | svn merge + svn:mergeinfo | ✅ | `b04433c` |
| 5.13 | 3-way conflict resolution + `svn resolve` | ✅ | (prev commit) |
| 5.14 | Server-side properties (propset round-trips through commit/checkout/update) | ✅ | (prev commit) |
| 5.15 | `svn switch` — relocate WC to a different branch URL, reusing update pipeline | ✅ | (prev commit) |
| 5.16 | `svn merge -c N` cherry-pick + reverse merge (`-r A:B` with A>B or `-c -N`) | ✅ | (prev commit) |
| 5.17 | `svn log -v` — per-rev A/M/D path list via new /rev/N/paths endpoint | ✅ | (prev commit) |
| 5.18 | mergeinfo arithmetic (cancel/collapse) + prop-delete propagation on update | ✅ | (prev commit) |
| 6.1  | Pluggable hash algorithms (sha1 golden-list default, sha256 via `--algos`) | ✅ | (prev commit) |
| 6.2  | Merkle verification: node-hash headers + `svn verify` re-hash walk | ✅ | (prev commit) |
| 7.1  | Authorization: out-of-line ACLs + `svn acl` CLI + Merkle redaction | ✅ | (prev commit) |
| 7.2  | Write-side ACL enforcement + copy guard; rw/r/w rule modes | ✅ | (prev commit) |
| 7.3  | `svn cleanup` — remove stale `.tmp.*` files + wc.db-journal | ✅ | `60589f5` |
| 7.4  | REST PUT/DELETE on `/path/<rel>` with Svn-Based-On concurrency token | ✅ | (prev commit) |
| 7.5  | Multi-algo secondary verification (`svn verify --secondaries`) | ✅ | (prev commit) |
| 7.6  | `svn blame` — per-line revision attribution (LCS-based) | ✅ | (prev commit) |
| 7.7  | `svn cp` refuse-unless-RW-everywhere + ACL auto-follow | ✅ | (prev commit) |
| 8.1  | Branch infrastructure + default `main` + path-rev index | ✅ | `795dfe8` |
| 8.2a | `svn branch create` + include-glob spec storage (no enforcement yet) | ✅ | `7a698e2` |
| 8.2b | Branch-spec enforcement on commits + cross-branch cp refusal | ✅ | `0f57c60` |
| 8.2c | First C→Aether port: branch-spec glob matcher (`spec.ae` + `aetherc --emit=lib`) | ✅ | (this commit) |
| 12 | svnadmin create/dump/load | ✅ | `52380a5` |

## Phases not yet done (from the plan)

| Phase | Title | Why deferred |
|------:|:------|:-------------|
| 0 item 5 | TLS transport (HTTPS) | Deferred per user direction. Server is plain HTTP today. |
| 8 (auth) | Basic/Bearer auth over TLS | Same — deferred. |
| 11 | libmagic replacement | Not on critical path; stub with extension-based mime detection when needed. |

## Feature matrix — reference svn vs svn-aether

Implemented:

- `checkout`, `update`, `switch`, `status`, `commit` (both URL-flag stateless and WC-backed)
- `add`, `rm`, `cp`, `mv`, `revert`, `diff`
- `log` (with `-v`/`--verbose` per-rev path list), `cat`, `ls`, `info` (all over the wire)
- `propset`/`get`/`del`/`list` (WC-local)
- `svn:ignore` read by status
- Server-side copy (`svn cp URL URL`) — full-subtree rep-sharing
- `svn merge` — forward rev-range (`-r A:B`, A<B), reverse (A>B), and
  cherry-pick (`-c N` or `-c -N` for reverse). Classic `URL@A:B` form
  still supported. Always uses 3-way merge so cherry-pick onto a tree
  the source never saw does the right thing.
- `svn:mergeinfo` recorded with range arithmetic: adjacent/overlapping
  ranges collapse (`src:5-5` + `src:6-6` → `src:5-6`), forward and
  reverse ranges of the same rev cancel out, empty results remove the
  property entirely.
- `svnadmin create/dump/load` with portable dump round-trip
- Per-repo pluggable content-address hash: sha1 (default) or sha256 via
  `svnadmin create PATH --algos sha256`. Format file records the choice.
  Server advertises via GET /info; clients adapt at checkout time and
  persist the algo in wc.db so pristine + change-detection use the same
  hash the server does. Golden list enforced at create time.
- Merkle verification end-to-end: server stamps every cat/list/props
  response with `X-Svnae-Hash-Algo`, `X-Svnae-Node-Kind`, and
  `X-Svnae-Node-Hash` headers. `svn verify URL [--rev N]` walks the
  tree top-down, re-hashes every file and dir blob locally, rebuilds
  each dir blob from its children's hashes, and confirms the
  recomputed root matches `/info`'s root sha. Detects tampering with
  stored .rep files.
- Authorization (placeholder auth): ACLs stored out-of-band in the
  rev blob as an `acl:` sha pointing at a paths-acl blob. Per-path
  ACLs are `+alice` / `-eve` / `+*` / `-*` rule lines, with optional
  mode suffix (`+alice:r` read-only, `+alice:w` write-only, `+alice:rw`
  = the shorthand `+alice`). Inheritance: nearest ancestor wins; no
  rule anywhere = open. `svn acl set/get/clear` manages via commits
  (super-user required). Server filters cat (404), list (blinds
  denied children as `{kind:hidden, sha}`), props, log entries, and
  paths endpoints. `/info` root sha is re-computed per user so
  `svn verify` anchors at the caller's *view* of the tree; Merkle
  verification still succeeds through hidden entries.
  Write enforcement: commit returns 403 on any denied path; self-
  elevation refused (user with no write on X can't set ACL on X);
  anonymous users (no header) can't write ACL'd paths. Copy guard:
  `svn cp URL URL` refuses unless the caller has RW on every path
  in the source subtree; super-user bypasses. On success, the
  source's paths-acl entries auto-follow — rebased onto the
  destination — so the branch inherits its source's restrictions
  verbatim (no accidental exposure).
- REST node editing: `PUT /repos/{r}/path/<rel>` with raw body
  updates or creates a file; `DELETE` same URL removes. Optimistic
  concurrency via `Svn-Based-On: <prior-sha>` header — 409 on
  mismatch with `X-Svnae-Current-Hash` in response for retry.
  Omitted header on PUT = create-if-absent. Passes through the
  write-ACL check. Optional `Svn-Author:` / `Svn-Log:` headers;
  defaults synthesized from user header + URL. Anyone with curl
  can script repo edits without a WC or our CLI. Auth is placeholder: `X-Svnae-User: <name>` header
  trusted verbatim, super-user proven by `--superuser-token SECRET`
  match (real auth deferred). Clients set `SVN_USER` /
  `SVN_SUPERUSER_TOKEN` env vars.

Not implemented (items the plan calls out or that reference svn has):

- HTTPS / TLS transport
- Real authentication (LDAP/AD/Basic — placeholder in place today)
- Hooks (pre/post-commit scripts)
- Locks (`svn lock`/`unlock`)
- `svn externals` (svn:externals property pulling sub-WCs)
- Pre-1.7 WC compatibility (intentionally dropped)
- Reference-svn dump-format compatibility (intentionally dropped;
  we have our own portable format)

## Where the code lives

```
ae/
  subr/           foundations — error, path, checksum, compress, io,
                  sqlite, utf8proc bindings. Each is a C shim + .ae
                  wrapper + a test.
  delta/          svndiff1 encode/decode, xdelta match-finder.
  fs_fs/          content-addressable blob store, revisions, txn,
                  server-side copy, commit finalisation.
  repos/          query API (log, cat, list, info) on top of fs_fs.
  ra/             REST client (libcurl + cjson).
  svnserver/      HTTP server wrapping repos/, plus per-route handlers
                  and the commit/copy POST endpoints.
  wc/             working copy: db, pristine, checkout, status,
                  mutate (add/rm), commit, update, revert, diff,
                  cp/mv, props, merge.
  svn/            the svn CLI.
  svnadmin/       svnadmin CLI (create/dump/load).
```

Each module has one or two C `shim.c` files (the real implementation)
and an Aether wrapper/driver. Tests are either in-language `.ae` or
`.sh` drivers that exercise the whole stack through built binaries.

## Design choices the plan gave us, and we kept

- **Clean-sheet storage:** not byte-compatible with reference svn's
  FSFS. Our `$repo/reps/aa/bb/<sha1>.rep` layout is simpler and uses
  content-addressable SHA-1 keys directly.
- **Single RA, no plugin loader:** one wire protocol, no `ra_serf` /
  `ra_svn` / `ra_local` multiplexing.
- **REST + JSON + base64:** no WebDAV/DeltaV. Our five endpoints fit
  on one screen of text.
- **System libraries via FFI:** libsqlite3, libz, libutf8proc, libssl,
  libcrypto, libcurl, libcjson. No vendored sources.
- **APR as reference only:** the `apr/` directory at the repo root is
  a read-only copy of the C portability layer we're *replacing* with
  Aether-native code + FFI. We never linked against it.

## Aether feedback

See `AETHER_ISSUES.md`. 16 numbered issues with repros, severity, and
workarounds. TL;DR: the type-system cluster (#1, #2, #13, #14, #16)
caused us to write ~4k lines of C shims where Aether logic would have
been idiomatic. Fixing that cluster unlocks rewriting most of the shim
code in Aether. Everything else is ergonomics.

## Running all tests

```bash
cd /home/paul/scm/subversion/subversion

AE=/home/paul/scm/aether/build/ae

# In-language .ae suites
for t in error path utf8 checksum compress io sqlite svndiff xdelta \
         repo revisions txn repos; do
  case $t in
    svndiff|xdelta) f=ae/delta/test_$t.ae ;;
    repo|revisions|txn) f=ae/repo_storage/test_$t.ae ;;
    repos) f=ae/repos/test_$t.ae ;;
    *) f=ae/subr/test_$t.ae ;;
  esac
  printf '%-27s ' "test_$t"
  "$AE" run "$f" 2>/dev/null | tail -1
done
for t in wc_db wc_pristine; do
  printf '%-27s ' "test_$t"
  "$AE" run "ae/working_copy/test_${t#wc_}.ae" 2>/dev/null | tail -1
done

# End-to-end shell suites (spin up a real server, hit it with curl + svn CLI)
for t in server ra svn wc_checkout wc_status wc_mutate wc_commit \
         wc_update wc_revert_diff wc_cp_mv wc_props wc_ignore \
         wc_branch wc_merge svnadmin; do
  printf '%-27s ' "test_$t"
  case $t in
    server)   s=ae/svnserver/test_server.sh ;;
    ra)       s=ae/client/test_client.sh ;;
    svn)      s=ae/svn/test_svn.sh ;;
    wc_*)     s=ae/working_copy/test_${t#wc_}.sh ;;
    svnadmin) s=ae/svnadmin/test_svnadmin.sh ;;
  esac
  "$s" >/dev/null 2>&1 && echo "test_$t: OK" || echo "test_$t: FAIL"
done
```

Expected: 30 lines, every one ending in `OK`.
