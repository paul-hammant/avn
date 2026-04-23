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
- **Round 7** (current): ported the svnserver /rev/N URL-tail parser
  (ae/svnserver/url_parse.ae), dropped four copies of hand-rolled
  `int_to_dec` + `digit_char` in favour of `std.string.from_int` now
  that it's available, and ported the WC pristine-store path builder
  to ae/wc/pristine_path.ae (handles the two-level XX/YY/ fanout).
- **Round 17** (current): **40.75% C, 59.24% Aether.** Finishing
  off the cJSON cleanup and picking up a few small fs_fs helpers:
  - Dead cJSON compat layers removed from ra/shim.c and
    svnserver/shim.c (both had no remaining users; kept as
    historical scaffolding from pre-Aether-stdlib-json days).
  - ra_parse_rev_response collapses three inline four-line
    cJSON {"rev":N} parsers into one Aether helper.
  - svnae_env_get → std.os::getenv.
  - svnae_branch_spec_allows → ae/fs_fs/branch_spec.ae.
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
    → ae/wc/update_props.ae. Opaque-handle walk via already-
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
    serialisation moves up to ae/ra/commit_build.ae. std.json
    for construction; C-side accessors expose struct svnae_ra_commit.
  - `svnae_ra_server_copy` + `svnae_ra_branch_create` bodies:
    small cJSON builders, also Aether via std.json.
  - verify_dir: swapped inline cJSON walker for the already-ported
    ra_parse_list packed-string parser.
  - `head_rev` + `rev_blob_sha1` → ae/repos/rev_io.ae.
  - `walk_remote` (svn update) → ae/wc/update_walk.ae.
  - `walk_remote` (svn merge) → ae/wc/merge_walk.ae.

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
  - RA client (ae/ra/shim.c) also got parse-side ports for
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
  - `filter_dir_recursive` (fs_fs, 89 lines of C → ae/fs_fs/filter.ae)
  - `rebuild_dir_c` (fs_fs, 292 lines of C → ae/fs_fs/rebuild.ae)
    — the core commit tree rebuilder that had been flagged as
    "weeks of work" earlier in the session
  - `compute_redacted_dir_sha` (svnserver, 50 lines → ae/svnserver/redact.ae)
  Plus two more algorithmic ports:
  - `svnae_wc_update` apply pipeline (wc, 135 lines → ae/wc/update_apply.ae)
  - `svnae_wc_merge` apply pipeline (wc, 135 lines → ae/wc/merge_apply.ae)
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
    repo|revisions|txn) f=ae/fs_fs/test_$t.ae ;;
    repos) f=ae/repos/test_$t.ae ;;
    *) f=ae/subr/test_$t.ae ;;
  esac
  printf '%-27s ' "test_$t"
  "$AE" run "$f" 2>/dev/null | tail -1
done
for t in wc_db wc_pristine; do
  printf '%-27s ' "test_$t"
  "$AE" run "ae/wc/test_${t#wc_}.ae" 2>/dev/null | tail -1
done

# End-to-end shell suites (spin up a real server, hit it with curl + svn CLI)
for t in server ra svn wc_checkout wc_status wc_mutate wc_commit \
         wc_update wc_revert_diff wc_cp_mv wc_props wc_ignore \
         wc_branch wc_merge svnadmin; do
  printf '%-27s ' "test_$t"
  case $t in
    server)   s=ae/svnserver/test_server.sh ;;
    ra)       s=ae/ra/test_ra.sh ;;
    svn)      s=ae/svn/test_svn.sh ;;
    wc_*)     s=ae/wc/test_${t#wc_}.sh ;;
    svnadmin) s=ae/svnadmin/test_svnadmin.sh ;;
  esac
  "$s" >/dev/null 2>&1 && echo "test_$t: OK" || echo "test_$t: FAIL"
done
```

Expected: 30 lines, every one ending in `OK`.
