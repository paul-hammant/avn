# TODO

Outstanding work on the svn-aether port. Prioritised by leverage.
Tests are 32/32 green at HEAD (bash integration suite) plus 8/8
Aether-native unit tests; 100% Aether (zero hand-written C); naming
sweep finished through Round 206.

## Real failures surfaced by Round 225

Five `test_*.ae` programs were declared as `[[bin]]` in aether.toml
but never actually run. When wired into the test suite via
`aether.program_test(b)`, they reveal real assertion failures.
The .ae sources are still in the tree as scaffolding; the
`.tests-<tag>.ae` wrappers are NOT in the aggregator (would break
the green build). Each needs its own debugging round.

### F1. ae/delta/test_svndiff.ae — 6 cases failing (svndiff is real, not dead)

**svndiff is production-bound**: it's the wire format for `svn diff`
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
varint). Filed as
`/home/paul/scm/aether/binary-string-extern-boundary.md`.

Once that lands, the existing test fix in HEAD passes verbatim
(test was already correctly using `string.length(got_str)` after
Round 226). Recreate `ae/delta/.tests-svndiff.ae` and add to the
aggregator.

**Test fix kept**: ae/delta/test_svndiff.ae now reads
`string.length(got_str)` for the length and `status` for the
encoder-status flag. The 6 cases still fail with `length 0 != N`
(was `length 1 != N`), surfacing the real bug more clearly.

### F2. ae/delta/test_xdelta.ae — multiple cases
**Symptom**: same shape (`length 1 != N`).
**Hypothesis**: shares the same length-accessor as F1.

### F3. ae/fs_fs/test_repo.ae — `read_blob` failures
**Symptom**: `read_blob big length: got 0, want 10000` and
content mismatches.
**Hypothesis**: blob read returns empty for large blobs.
Could be related to F1/F2 if blob length accessor regressed.

### F4. ae/fs_fs/test_revisions.ae — file-content mismatches
**Symptom**: `r1 README:` fails (left empty), same for main.c
and notes.txt.
**Hypothesis**: read path issue — possibly the same
length-accessor or rev-blob seeding contract broke.

### F5. ae/repos/test_repos.ae — at least 2 cases
**Symptom**: `cat r3 LICENSE: expected null/missing, got something`
— pre-r3 file should not exist, but does.
**Hypothesis**: rev resolution returns the wrong content for
deleted-in-rev paths.

To debug: pick one (F1 is the smallest), run
`aeb ae/delta/.tests-svndiff.ae` (need to recreate the file —
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
muscle-memory `svn co` and surprise them.
**Shape**: `ae/svn/test_aliases.sh` — for each alias pair, run
both forms against the same in-process server, diff stdout/stderr.

### T2. `svn help` and unknown-subcommand error path (single test, ~10 lines)
**Why**: `svn help` should print usage and exit 0; `svn nosuchcmd`
should print an error and exit non-zero. Neither path is exercised
by any test today.
**Shape**: `ae/svn/test_help.sh`.

### T3. paths_index_lookup empty-path branch (unit test in test_*.ae form)
**Why**: Round 206 merged `paths_index_lookup_impl` into the
exported `paths_index_lookup`, adding an extra branch that handles
empty `path` (the "<sha> \n" entry shape used for root-level ACL
rules). ACL tests hit this path indirectly when walking down to
root, but the branch isn't exercised in isolation.
**Shape**: `ae/svnserver/test_paths_index.ae` — build a fixture
body, call `paths_index_lookup(body, "")` and assert the right sha
is returned.

### T4. Concurrent server requests (1 test, ~50 lines)
**Why**: every existing server test is sequential. The
aether-svnserver is multi-threaded under std.http; an actor-related
data race would survive every existing test. `std.config`'s
reader/writer lock is the kind of thing that would hide a bug here.
**Shape**: `ae/svnserver/test_concurrent.sh` — spawn server, fire
20 parallel `curl` GETs at /repos/X/log, assert all return 200 with
matching bodies. (Cheap insurance against a lock regression.)

### T5. Round-trip equivalence: dump → load → re-dump (1 test, ~40 lines)
**Why**: `test_svnadmin.sh` tests dump and load separately. A
round-trip property check would catch dump-format bugs that
preserve loadability but mutate content.
**Shape**: `ae/svnadmin/test_roundtrip.sh` — seed a repo, dump it,
load into a fresh repo, dump that, assert byte-identical dumps.

---

## Naming follow-ups

### N1. Compound `*_with_X` exports — DONE (Rounds 208-209)
All three `_with_X` overload-style exports collapsed into their
non-`_with` siblings:
- `commit_finalise(repo, txn, branch, author, log, props_sha, acl_sha)` — Round 208
- `svnadmin_create(repo, algos_spec)` — Round 209

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
Would unlock the `binbuf.slice(...)` / `Card.add()` ergonomics.
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
