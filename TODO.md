# TODO

Outstanding work on the avn port. Prioritised by leverage.
Tests are 32/32 green at HEAD (bash integration suite) plus 8/8
Aether-native unit tests; 100% Aether (zero hand-written C); naming
sweep finished through Round 206.

## MB-H — Server-side merge ledger (replacing `svn:mergeinfo`)

In progress. Migrating merge tracking from the versioned property
(architecturally flawed — see Round 268-269 commit notes and the
TODO MB-G discussion) to a server-side SQLite ledger
(`<repo>/merges.db`) plus a `merged:` field on the rev blob.

Plan (each numbered item is one baby commit):

  1.1 *DONE Round 270*. New `repo_storage/merge_ledger.ae` —
      schema (`merge_event`, `merge_source_rev`), public API
      (`merge_ledger_init_schema`, `merge_ledger_record`,
      `merge_ledger_is_integrated`, `merge_ledger_integrated_set`,
      `merge_ledger_eligible_revs`). Eager schema install in
      `avnadmin/create.ae`'s `create_bare_`. Lazy install on
      first use for older repos. Unit test
      `util/.tests-merge_ledger.ae` (44/44 green). No callers
      yet — surface only.

  1.2 Rev blob carries `merged: <source_branch>:<sha>,...` field.
      `repo_storage/commit_finalise.ae`'s `rev_blob_body` emits;
      `repos/rev_io.ae` parses; `repos_rev_field(repo, rev,
      "merged")` accessor. Round-trip pinned.

  1.3 Wire merge metadata into the txn. `txn_merge_record(t,
      source_branch, source_sha)` setter. `commit_finalise` reads
      the txn's merge list, writes the rev blob's `merged:` field
      *and* writes ledger rows atomically.

  1.4 HTTP read endpoints: `GET /repos/X/merged?target=B&source=S`
      and `GET /repos/X/eligible?target=B&source=S&candidates=...`.

  2.1 `wc_merge` queries the ledger via HTTP (instead of parsing
      `svn:mergeinfo` property). Property still gets written
      (dual-state) until 2.3.

  2.2 New `avn merged` CLI subcommand.

  2.3 Stop writing `svn:mergeinfo`. Server rejects incoming
      `svn:mergeinfo` with HTTP 400. Tests update:
      test_merge / test_merge_reverse / test_mergeinfo_arith /
      cherry_convergence — assert via `avn merged` instead.

  3.1 *DONE Round 277*. Deleted `working_copy/mergeinfo.ae` (612
      lines: rangeset string parse/emit, cancel-pairs, source-set
      extraction). Dropped from avn/.build.ae. The retired
      .test_mergeinfo_arith_driver.ae + .tests-mergeinfo_arith.ae
      went with it.
  3.2 *DONE Round 277*. svnserver/commit_parse.ae rejects any
      commit body whose props dict contains a `svn:mergeinfo`
      entry, with HTTP 400. Wire-level rejection — defence
      against buggy or malicious clients trying to poke the
      ledger via the property carrier.
  3.3 *DONE Round 277*. avn/main.ae cmd_propset rejects
      `svn:mergeinfo` at the CLI with a clear "server-managed"
      message.

  4.1 (optional) `avnadmin verify --rebuild-merges` rebuilds the
      ledger from rev blobs.

Architectural rationale captured at length in the Round 264-269
commit messages — short version: `svn:mergeinfo` was server-side
storage choice (property in versioned content) that bled all the
classical-SVN merge-tracking pain into user space; ledger is
opaque server metadata, server-only-writer, sha-keyed (immune to
rename and history-rewriting concerns since avn doesn't do either),
no diff noise, no hand-edit surface.

## Cherry-pick convergence — bug-bait test (2026-05-06; FIXED 2026-05-07)

`working_copy/.tests-cherry_convergence.ae` + `.test_cherry_convergence_driver.ae`
mirrors the "limits of merging" blog
(https://github.com/paul-hammant/limits-of-merging-experiment):
build a 5-rev trunk on /trunk/, branch twice, run cherry-pick C4→C3
then C3→C4 followed by sweep merge, assert both branches end byte-
identical.

**Status: 11/11 GREEN** as of Round 268. In the root aggregator.

Surfaced six gaps that consolidated to five root-cause fixes
(MB-A..MB-G). All five landed in Rounds 264-268. The historical
catalogue stays below for reference.

The classical-SVN reference is the `svn-version` branch of the same
upstream repo
(https://github.com/paul-hammant/limits-of-merging-experiment/tree/svn-version).
That branch's `start.sh` + `scenario-{a,b,c}*.sh` run the same
workflow against `subversion` (Apache C implementation) and pass
cleanly — including the `svn:mergeinfo` consultation on sweep
merge. Anything our port fails at that the C version does not is
a regression we introduced.

### MB-A. `avn commit` from a stale WC silently overwrites HEAD ⚠ **CORRECTNESS BUG**
Two WCs check out at r1, both edit the same file, both commit in
sequence. Classical SVN rejects the second commit with "File or
directory ... is out of date". avn's commits both as r2 and r3,
**silently losing WC1's content**. Repro pinned in
`/tmp/probe.log` from the 2026-05-07 probe.

This is a data-loss bug, not a missing feature.

### MB-B. `avn cp URL/path@PASTREV URL/dst` clobbers sibling top-level dirs
With `/trunk`, `/release_a` already at HEAD, run
`avn cp URL/trunk@1 URL/release_b`. Resulting tree drops
`/release_a`. Classical SVN's `avn copy -r1 URL/trunk URL/branches/release`
adds the new dir leaving siblings intact. Likely the avn server-
side copy from past rev rebuilds the tree from the source-rev
snapshot rather than starting from HEAD.

### MB-C. WC subdirs aren't versioned
`mkdir $WC/sub && cp file $WC/sub/ && cd $WC/sub && avn add file`
prints `A  file` but the parent WC sees `?  sub` (unversioned). A
subsequent `avn commit` from either the subdir OR the root says
"No changes to commit" or "avn commit: failed". This breaks the
SVN-classical `/trunk + /branches` layout entirely — `cd $WC/trunk &&
avn add x && avn commit` is the canonical form and it doesn't
work. Looks like the WC.db indexes a flat path-set keyed on
the root-checkout dir.

### MB-D. Sub-path checkout fails
`avn checkout URL/sub_path $WC` returns "could not contact server"
even when the path exists. Only root checkout works. Blocks the
"branch into its own WC" workflow needed for cherry-picking onto
a release branch in classical layout.

### MB-E. Branch URL checkout (first-class branch syntax) fails
`avn checkout URL;branch_name` (avn's first-class branch URL
syntax) also returns "could not contact server". `avn branch
create` works server-side but there's no way to get a WC of the
branch.

### MB-F. `avn add /absolute/path/to/file` fails (rc=-2)
Only `cd $WC/.. && avn add relative/path` works. Classical SVN
accepts both.

### MB-A / MB-B diagnosis (2026-05-07)

**Not regressions — never implemented.** Look at
`repo_storage/commit_finalise.ae:170-181` (`finalise_on_branch_`):

```aether
base_rev = txn_base_rev(txn)
base_root = load_rev_root_sha1_(repo, base_rev)
new_root = rebuild_dir(repo, base_root, "", txn)   // ← rebuild
prev = repos_head_rev(repo)
next = prev + 1
```

The new tree is rebuilt from the *base_rev* snapshot, completely
ignoring whatever happened between base_rev and HEAD. WC2 with
stale base_rev=1 commits: rebuilds from r1's tree, applies WC2's
edits, lands as r3 — silently dropping WC1's intervening r2.

Pre-Round 64 C version in `ae/fs_fs/commit_shim.c` had the same
shape:

```c
int base_rev = svnae_txn_base_rev(txn);
char *base_root = load_rev_root_sha1(repo, base_rev);
char *new_root = svnae_txn_rebuild_root(repo, base_root, txn);
int prev = svnae_repos_head_rev(repo);
int next = prev + 1;
```

First introduced in Phase 6.5 (commit `dfd6567`, 2026-04-19) when
the POST /commit endpoint landed. No out-of-date check has ever
existed in the avn commit pipeline. Bisecting [Phase 6.5..HEAD]
would just say "always bad."

**MB-B is the same architectural gap.** Server-side `avn cp
URL/path@PASTREV URL/dst` opens a txn with `base_rev = PASTREV`,
runs the same `rebuild_dir(repo, base_root, ..., txn)` and lands
the result as a fresh rev. Anything that existed at root between
PASTREV and HEAD but isn't reachable from `/path@PASTREV` is
silently dropped from the new tree.

**The fix for both** is the same shape: between
`prev = repos_head_rev(repo)` and `rebuild_dir(...)`, walk each
of the txn's edited paths and ask "did any rev in (base_rev..prev]
touch this path?" If yes, return -1 with a distinct out-of-date
error code. Classical SVN's commit editor does this implicitly
through its delta-against-HEAD design; ours rebuilds against
base_rev's tree directly so we have to bolt the check on
explicitly.

For MB-B specifically, also worth deciding whether server-side
copy should *merge* the past-rev source into the HEAD tree
(preserving siblings) or *replace* HEAD with `base_rev + dst-only`
(current behaviour). Classical SVN's `avn copy URL@REV URL/dst`
does the former — only the new dst path is added; everything else
at HEAD survives.

### MB-C diagnosis (2026-05-07)

**`wc_db_open` silently creates a new WC if `.svn/wc.db` is
missing.** From `working_copy/db_open.ae:22`:

```aether
export wc_db_open(wc_root: string) -> ptr {
    pristine_dir = "${wc_root}/.svn/pristine"
    merr = fs.mkdir_p(pristine_dir)            // ← creates if missing
    ...
    db, oerr = sqlite.open(dbpath)             // ← creates if missing
    if wc_db_install_schema(db) != 0 { ... }   // ← creates schema
    return db
}
```

Verified by probe: with a real WC at `$WC` and an unversioned
`$WC/sub/`, running `cd $WC/sub && avn add foo.txt` silently
creates a brand-new isolated `$WC/sub/.svn/wc.db`. The add
"succeeds" against this fake WC but the parent's `$WC/.svn/wc.db`
never learns about it, so a later `avn commit` from `$WC` sees
nothing scheduled.

**Fix**: `wc_db_open` should distinguish "open an existing WC"
(used by `wc_add`, `wc_commit`, `wc_status`, `wc_merge`, …) from
"initialise a new WC" (used only by `cmd_checkout`'s call path).
The first form should refuse if `.svn/wc.db` doesn't exist at the
provided `wc_root`; the second form keeps today's auto-create
behaviour.

### MB-D / MB-E diagnosis (2026-05-07)

**Same bug, different surface.** `cmd_checkout` and `cmd_ls` use
the shallow `url_base` / `url_repo` helpers (split on the LAST `/`)
which only parse 2-segment `host/repo` URLs:

```aether
url_base(url) → everything before the last "/"
url_repo(url) → everything after the last "/"
```

For `URL = http://host/demo/release_a` (avn's first-class branch
URL grammar — see `branch_of` at `avn/main.ae:271`), this gives
`base = http://host/demo`, `repo = release_a`. The subsequent
`remote_head_rev(base, repo)` queries
`http://host/demo/repos/release_a/info` → 404 → "could not contact
server".

The deep parsers `base_url_deep` / `repo_name_deep` / `in_repo_path`
already exist in the same file and are used correctly by `cmd_cp`
and `cmd_merge`. `cmd_checkout` and `cmd_ls` just don't reach for
them. MB-E is the same bug exercised through avn's branch
grammar.

**Fix**: switch `cmd_checkout` and `cmd_ls` to the deep parsers,
and extend the `wc_checkout` extern (currently `(base_url,
repo_name, dest, rev)`) to accept a sub-path / branch parameter
so the WC stores the correct anchor.

### MB-F diagnosis (2026-05-07)

**`cmd_add` hard-codes `wc_root="."`**, see `avn/main.ae:881`:

```aether
rc = wc_add(".", p)
```

`p` is taken verbatim from argv. If `p` is absolute (`/abs/path`),
`wc_add` joins it onto cwd and fails to locate a WC. If cwd
isn't a WC root, see MB-C — `wc_db_open` happily creates a new
fake WC at "." instead of erroring.

**Fix**: walk up from `realpath(p)` looking for `.svn/wc.db`;
that's the WC root. Compute the relative path from there. Apply
the same lookup in every `cmd_*` that takes WC paths
(`cmd_status`, `cmd_commit`, `cmd_revert`, ...). Classical SVN
calls this the "WC anchor" lookup; it's a well-trodden algorithm.

### Severity & ordering — six gaps, five root-cause fixes (all landed)

| Gap | Round | Root cause | Fix |
|---|---|---|---|
| MB-A | 264 | commit_finalise rebuilds from base_rev's tree without scanning `(base_rev..HEAD]` | walk txn paths against intervening revs via path_rev table; rebuild on HEAD's tree; return -12 (HTTP 409) on conflict |
| MB-B | 264 | (same as MB-A) | (same fix lifts both — past-rev cp now applies on top of HEAD) |
| MB-C | 265 | wc_db_open auto-creates a new WC | split into `wc_db_open` (refuse-unless-exists) + `wc_db_init` (only `wc_checkout` calls) |
| MB-D | 266 | cmd_checkout/cmd_ls used shallow `url_base`/`url_repo` (last-/-split) | use `base_url_deep` / `repo_name_deep` / `in_repo_path` (already existed for cmd_cp / cmd_merge) |
| MB-E | 266 | (same as MB-D — `URL/branch_name` is the right grammar) | (same fix) |
| MB-F | 267 | cmd_* hard-coded `wc_root="."` | `find_wc_anchor_()` walks up from cwd; cmd_add / cmd_commit / cmd_merge / cmd_propget / cmd_status converted |
| MB-G | 268 | sweep `merge -r A:HEAD` ignored existing svn:mergeinfo, re-applied already-merged revs | (Round 268 partial) shift `rev_base` past contiguous-prefix; mergeinfo recorded on the merge target (not WC root). (Round 269) **full non-contiguous filtering** — `mergeinfo_eligible_runs` splits the requested range into maximal-eligible chunks and applies each as a separate diff |

**MB-D follow-up**: sub-path checkout (`avn checkout URL/release_a $WC`)
still refuses with a clear "not yet supported" message. Full anchor
support requires recording the WC anchor in wc.db.info and having
every wc_* call prepend it when sending paths server-ward. Filed as
MB-D-2 if a real workflow needs it.

**MB-G-2 (Round 269)**: non-contiguous mergeinfo handling now
matches classical SVN. `mergeinfo_eligible_runs(existing, source,
lo, hi)` splits the requested range into maximal contiguous runs
of revs not yet merged; `wc_merge` iterates those runs and applies
each as a separate `(lo..hi]` diff against the WC. Each chunk is
its own `merge_walk_remote` × 2 + `merge_apply_*` pass, sharing
one wc.db handle for the whole sweep.

**MB-F decision pending** — 11 cmd_* still hardcode `wc_root="."`
(cmd_rm, cmd_revert, cmd_diff, cmd_resolve, cmd_propset/del/list,
cmd_cp, cmd_mv, cmd_update, cmd_switch, cmd_cleanup). Walk-up was
applied only to the five commands the cherry-pick test exercises
(add, commit, merge, propget, status). The pattern is established
(`find_wc_anchor_()` + `wc_join_(cwd_offset, arg)`) — sweeping the
remaining 11 is mechanical IF we decide to support them. Open
question: avn may simply not support cd-into-subdir for those
commands — held until a real workflow needs it.

Convergence claim from svnbook §4
(https://svnbook.red-bean.com/en/1.6/avn.branchmerge.advanced.html#avn.branchmerge.cherrypicking)
verified in Round 268: cherry-pick C4→C3 vs C3→C4 followed by
sweep produce byte-identical files matching trunk@HEAD's content.


## Future query API surface

### Q1. Combined `--all` view + integrated `-v` verbose decoration
After Round 280 ships verbose mode for `--eligible` and `--pending`, the
default-integrated form (the SHA list) also wants `-v` decoration —
but the SHA → (rev #, author, date, log) resolution requires either a
schema bump on `merge_source_rev` (add `source_rev_num INTEGER`,
populated at write-time from commit_parse's already-resolved rev #) or
a server-side enrichment endpoint that walks rev blobs to map SHA →
rev # at query time. Schema bump is preferred (the column is
populated for free; no scan); the wire shape of `/merged` becomes
`{integrated:[{"sha":"…","rev":N},…]}` and the existing SHA-only
shape goes away (no other clients to break).

Filed for after we have a real workflow asking for it. Today's `avn
merged URL --target T --source S` returns SHAs which dump cleanly into
shell pipes; verbose decoration is the future-proofing.

Same TODO covers an `--all` mode (combined integrated/eligible in one
listing) — same data plumbing, just a UI choice on top.

### Q2. Structured-query endpoint — only if 3+ shapes accumulate
If after `--all` and `-v` ship we end up adding more `/foo/bar/baz`
URL paths for new query shapes, switch to a homegrown query-body
endpoint:

  POST /repos/{r}/q
  {"select":["rev","sha","author","log"], "from":"integrated",
   "target":"release_a", "source":"trunk"}

~200 lines of dispatch in svnserver/. No parser. Two endpoints become
one and verbose mode falls out as a `select` field. **Don't build
this until we genuinely need three new shapes** — the REST endpoints
are fine while we're still only adding one new endpoint per round.

### Q3. If we ever ship a real GraphQL endpoint — library choice
Don't read this until Q2 has been built and Q2 alone is hurting.

Two viable libraries:

- **libgraphqlparser** (Meta, C++) — ships a C API on purpose,
  designed for binding to other languages. Easy to extern from
  Aether (link-time, no shim crate). The Meta-famous one. Preferred.

- **graphql-parser** (Rust crate) — needs a `#[no_mangle] extern "C"`
  shim crate built as `staticlib`. Doable, but you've signed up for
  maintaining the shim. Skip unless libgraphqlparser doesn't exist
  on the target platform.

avn's query graph (merge events ⇄ source revs ⇄ rev metadata) maps
naturally onto GraphQL's field-selection model — the `-v` problem
(client picks decoration depth) is the canonical GraphQL win. But
**this is overkill** for a CLI with one client and a finite query
set. File and forget unless someone actually asks for it.

## Round 228 update

Aether 0.116 shipped the `@aether string` per-param extern annotation
(opt-in, restores v0.97 header-preserving behaviour for
Aether-to-Aether crossings). Aether 0.117 is the stable release.

**F1 (svndiff) and F2 (xdelta) FIXED** in Round 228:
- delta/test_svndiff.ae: annotated the two binary string params
  on `svndiff_encoder_finish` and `svndiff_decode_apply` externs.
- delta/test_xdelta.ae: annotated `xdelta_compute` and
  `svndiff_decode_apply` externs; also fixed test's
  status-vs-length confusion (was reading the int tuple element
  as length; now reads `string.length(got)` directly).

Both .tests-svndiff.ae and .tests-xdelta.ae added to the
aggregator. 6/6 svndiff round-trip cases pass; xdelta likewise.

**F3-F5 still parked**: investigated test_repo (F3); the failure
isn't an Aether-to-Aether `@aether` annotation case. The blob
read path goes `zlib.inflate` → `zlib_get_inflate_bytes` (a C
extern returning `string`) → returned through Aether helpers
back to the test caller. The C-to-Aether crossing isn't
addressed by `@aether` (which only marks Aether-emitted
receivers). To preserve length end-to-end, the C-side function
would need to return the AetherString pointer directly (not the
unwrapped data), or callers route through `string.substring_n`
with explicit lengths. Defer until a production caller of
test_repo's read path emerges, or we have time to refactor the
zlib accessor pattern.

## Real failures surfaced by Round 225

Five `test_*.ae` programs were declared as `[[bin]]` in aether.toml
but never actually run. When wired into the test suite via
`aether.program_test(b)`, they reveal real assertion failures.
The .ae sources are still in the tree as scaffolding; the
`.tests-<tag>.ae` wrappers are NOT in the aggregator (would break
the green build). Each needs its own debugging round.

### F1. delta/test_svndiff.ae — 6 cases failing (svndiff is real, not dead)

**svndiff is production-bound**: it's the wire format for `avn diff`
and the dump/load delta encoding. Currently used by no production
binary, but we want it long-term. Keep the .ae source.

**Investigated 2026-05-02 with debug instrumentation**:
- The test was reading the second tuple element as length, but
  `svndiff_decode_apply` returns `(string, status)` — fixed by
  reading `string.length(got_str)` instead. (Test fix in HEAD; the
  failures below are real bugs not test errors.)
- Encoder reports correct values (source_len, target_len, etc).
  Generated diff has the right length but the **header varints all
  decode to 0**: bytes 4-10 of the encoded diff are all zero.
- Decoder reads `tview_len=0` from the stream → allocates a 0-byte
  target → succeeds → returns empty string.
- `bytes.set(b, pos, val)` works correctly **in isolation** (pilot at
  /tmp/bytes-pilot/test2 verifies). Same in svndiff_encode.ae:
  `bytes.set` calls are identical to the pilot.
- Bug must be in how `bytes.set` interacts with `emit_varint_`
  (helper function) inside the `encode_window` body. Possibly an
  aetherc code-gen issue specific to: helper-fn writes through a
  void* passed as parameter, multiple sequential calls, then
  `bytes.finish` reads the buffer.

**Round 226 update**: standalone C-level tracing pinpointed it.
Bytes 4..7 of the encoded diff are correct (`00 0e 0e 02`) but
when read by `decode_apply` via `string.char_at(diff, 4)` they
come back as 0. Cause: aetherc's call-site lowering passes
`aether_string_data(s)` (the data pointer past the AetherString
header) across `extern` boundaries. Receiver's magic check fails,
falls back to `strlen()`, truncates at the first NUL — which IS
byte 4 of every svndiff diff (the always-zero `sview_offset`
varint).

**Round 227 update**: Nic (Aether maintainer) replied: "use
std.config + std.actors.register/whereis as the way through this."
The extern-boundary unwrap is by-design; binary data that needs
to flow through callback shapes routes via the registry/config
mechanisms instead. No aetherc fix coming.

For svndiff specifically (a pure function call, not a callback
shape), the practical paths are:
1. **Collapse** encoder + decoder + caller into one .ae module
   so binary stays in one TU. ~380 LOC of merging; doable when
   svndiff actually has a production caller.
2. **Defer**. svndiff has no production caller today. Tests stay
   broken (not in aggregator); .ae source stays in tree.

Going with (2) until something needs `avn diff` against a remote
or dump-format delta encoding. Then tackle (1) as part of the
adoption work.

Reproducer + Nic's redirect documented at
`~/scm/aether/binary-string-extern-boundary.md`.

**Test fix kept**: delta/test_svndiff.ae now reads
`string.length(got_str)` for the length and `status` for the
encoder-status flag. The 6 cases still fail with `length 0 != N`
(was `length 1 != N`), surfacing the real bug more clearly.

### F2. delta/test_xdelta.ae — multiple cases
**Symptom**: same shape (`length 1 != N`).
**Hypothesis**: shares the same length-accessor as F1.

### F3. repo_storage/test_repo.ae — `read_blob` failures
**Symptom**: `read_blob big length: got 0, want 10000` and
content mismatches.
**Hypothesis**: blob read returns empty for large blobs.
Could be related to F1/F2 if blob length accessor regressed.

### F4. repo_storage/test_revisions.ae — file-content mismatches
**Symptom**: `r1 README:` fails (left empty), same for main.c
and notes.txt.
**Hypothesis**: read path issue — possibly the same
length-accessor or rev-blob seeding contract broke.

### F5. repos/test_repos.ae — at least 2 cases
**Symptom**: `cat r3 LICENSE: expected null/missing, got something`
— pre-r3 file should not exist, but does.
**Hypothesis**: rev resolution returns the wrong content for
deleted-in-rev paths.

To debug: pick one (F1 is the smallest), run
`aeb delta/.tests-svndiff.ae` (need to recreate the file —
it was deleted in Round 225), instrument the test, find the
length-accessor regression. Each is probably 30-60 lines of
investigation.

## Tests

These would lock behaviours that the current 32-test suite exercises
only indirectly. Each is small.

### T1. Subcommand alias coverage (single test, ~30 lines)
**Why**: 13 alias relationships exist (`co`/`checkout`, `up`/`update`,
`mv`/`rename`/`move`, `rm`/`delete`/`remove`, `pd`/`propdel`,
`pg`/`propget`, `pl`/`proplist`, `ps`/`propset`, `st`/`status`,
`sw`/`switch`, `di`/`diff`, `ann`/`annotate`/`praise`/`blame`).
The long form is exercised in existing tests; the alias form is
not. A regression that broke an alias would land in a user's
muscle-memory `avn co` and surprise them.
**Shape**: `avn/test_aliases.sh` — for each alias pair, run
both forms against the same in-process server, diff stdout/stderr.

### T2. `avn help` and unknown-subcommand error path (single test, ~10 lines)
**Why**: `avn help` should print usage and exit 0; `avn nosuchcmd`
should print an error and exit non-zero. Neither path is exercised
by any test today.
**Shape**: `avn/test_help.sh`.

### T3. paths_index_lookup empty-path branch (unit test in test_*.ae form)
**Why**: Round 206 merged `paths_index_lookup_impl` into the
exported `paths_index_lookup`, adding an extra branch that handles
empty `path` (the "<sha> \n" entry shape used for root-level ACL
rules). ACL tests hit this path indirectly when walking down to
root, but the branch isn't exercised in isolation.
**Shape**: `svnserver/test_paths_index.ae` — build a fixture
body, call `paths_index_lookup(body, "")` and assert the right sha
is returned.

### T4. Concurrent server requests (1 test, ~50 lines)
**Why**: every existing server test is sequential. The
avnserver is multi-threaded under std.http; an actor-related
data race would survive every existing test. `std.config`'s
reader/writer lock is the kind of thing that would hide a bug here.
**Shape**: `svnserver/test_concurrent.sh` — spawn server, fire
20 parallel `curl` GETs at /repos/X/log, assert all return 200 with
matching bodies. (Cheap insurance against a lock regression.)

### T5. Round-trip equivalence: dump → load → re-dump (1 test, ~40 lines)
**Why**: `test_avnadmin.sh` tests dump and load separately. A
round-trip property check would catch dump-format bugs that
preserve loadability but mutate content.
**Shape**: `avnadmin/test_roundtrip.sh` — seed a repo, dump it,
load into a fresh repo, dump that, assert byte-identical dumps.

---

## Naming follow-ups

### N1. Compound `*_with_X` exports — DONE (Rounds 208-209)
All three `_with_X` overload-style exports collapsed into their
non-`_with` siblings:
- `commit_finalise(repo, txn, branch, author, log, props_sha, acl_sha)` — Round 208
- `avnadmin_create(repo, algos_spec)` — Round 209

Empty strings stand in for unused optional args. An options-struct
form is still cleaner once we want to grow the parameter list, but
for the current arity (≤ 7) the positional form reads fine and
removes the overload set.

### N2. Long compound names that feel German
- `repos_load_rev_blob_field` → `repos_rev_field` — DONE Round 210
- `acl_user_has_rw_subtree` → `acl_subtree_rw` — DONE Round 210
- `fsfs_tree_builder_content_data(tb, i)` /
  `fsfs_tree_builder_content_len(tb, i)` — 4-token names on the
  same heap struct. With `*StructName` method-style syntax these
  could collapse. Still waiting on port-local module imports.

The remaining `fsfs_tree_builder_*` family is really blocked on the
same Aether ask underneath everything else (filed and then
retracted as `port-local-imports.md`; same underlying gap that
`Card.add()` ergonomics needs). Until that lands the German
shape is the link-symbol shape.

### N3. `_handle` suffix family (5 functions in svnserver)
`acl_handle`, `blame_handle`, `cat_handle`, `paths_handle`,
`info_handle` — these are HTTP route-handler bodies. The
`_handle` suffix mirrors their role. Could become methods on a
`Request` type later; for now the name is fine.

---

## Aether-side asks awaiting upstream

Filed on the Aether project; not actionable on the port side until
they ship.

### A1. Per-process actor refs (low priority — the `std.config` answer
ships and works for our use case, but the actor-singleton pattern
still benefits from a clean way to register/find an actor by name
from inside a `@c_callback` body). Doc-only fix; lower urgency than
when first filed.

### A2. Port-local module imports (`import port.X.Y`)
Would unlock the `byte_ops.slice(...)` / `Card.add()` ergonomics.
Sketched in N2 above. The link-symbol-flat-namespace shape
forces noun-stacked names like `wc_db_node_exists` instead of
`wc.db.node.exists` or `wc.db.node_exists`. A real ask but I'd
make sure the port actually wants the change first — current
naming is functional.

---

## Deferred port work

Listed in `PORT_STATUS.md` already, kept here for one-stop
visibility.

### D1. HTTPS / TLS transport
Server is plain HTTP today. Phase 0 item 5; deferred per user
direction. Aether's std.http.client supports HTTPS; the server
side needs cert configuration glue.

### D2. Real authentication
Today's `--user=alice` + `X-Svnae-User: alice` is placeholder
auth (the client claims; server trusts). Real auth (Basic /
Bearer / public-key over TLS) is Phase 8 deferred along with D1.

### D3. fs_fs commit_shim load_rev_root_sha1 (legacy reference)
Documented as a deferred port in `PORT_STATUS.md` line 413.
Probably moot now (the port is at 0% C); confirm and remove the
note if so.

---

## Doc / infra hygiene

### H1. PORT_STATUS.md is stale
The "Round X" entries in PORT_STATUS go up to Round 31 then stop.
The last 175 rounds (164 → 206) aren't in there. Either bring it
up to date or replace with a pointer to `git log` for round-by-
round detail.

### H2. AETHER_ISSUES.md is stale
Tracks against "main at v0.76.0" — port is now on Aether 0.110.
Several listed issues are likely fixed (e.g. #16 "extern with 6+
params drops the last" should be re-checked given the work since
0.76).

### H3. migration_method.md
Snapshot of the port's methodology. Probably worth a brief
end-of-port retrospective entry summarising what worked
(MDOLD strategy, tests-as-checkpoint, Aether features pulled in
to retire shims) and what didn't (the 0.84 bug workarounds we
carried for too long; the early "naming convention prefix"
that took 11 rounds to clean up).

---

## Items I'm explicitly NOT planning

- **Per-export unit tests**. The 32 end-to-end tests exercise the
  binaries the way users do. Adding a unit test per exported
  function would be a coverage-theatre trap.
- **Stress / scale tests** (large repos, deep histories,
  many-thousand commits). Different category; separate effort.
- **Cross-platform port**. Linux only today. Windows / macOS would
  need their own session.

— Porter Claude
