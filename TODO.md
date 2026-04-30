# TODO

Outstanding work on the svn-aether port. Prioritised by leverage.
Tests are 32/32 green at HEAD; 100% Aether (zero hand-written C);
naming sweep finished through Round 206.

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

### N1. Compound `*_with_X` exports
Three exports remain with the `with_X` overload-style suffix:
- `commit_finalise_with_acl(repo, txn, author, log, props_sha, acl_sha)`
- `commit_finalise_with_props(repo, txn, author, log, props_sha)`
- `svnadmin_create_with_algos(repo, algos_spec)`

Each is a sibling of a non-`_with` form. The pattern is C-style
overload disambiguation. Cleaner: a single `commit_finalise(repo,
txn, opts)` taking an options struct (Aether 0.109 `*StructName`
heap structs make this ergonomic), with `acl_sha` / `props_sha`
optional fields. Same for `svnadmin_create`.

### N2. Long compound names that feel German
A few exports still read like Win32 APIs:
- `repos_load_rev_blob_field(repo, rev, key)` → could be
  `rev_blob.field(rev, key)` if we had per-file imports
- `acl_user_has_rw_subtree(repo, rev, user, path)` → 5-token name
- `fsfs_tree_builder_content_data(tb, i)` /
  `fsfs_tree_builder_content_len(tb, i)` — 4-token names on the
  same heap struct. With `*StructName` method-style syntax these
  could collapse.

These all wait for **port-local module imports** (filed and then
retracted as `port-local-imports.md`; really it's the same
underlying gap that `Card.add()` ergonomics needs). Until that
lands the German shape is the link-symbol shape.

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
