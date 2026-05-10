# Methodical extern → import migration plan for avn

avn currently has **1319 `extern` declarations** across **132 `.ae` files** —
of which **507 (~40%)** are forward declarations of Aether functions defined
in *other* `.ae` files within avn itself. These are not bindings to C; they're
the residue of the C-to-Aether port done one-function-at-a-time, where each
function migration left an `extern` declaration behind in every caller so the
half-Aether-half-C codebase could keep linking.

By comparison, **aeb** (a fully-ported, idiomatic-Aether project of similar
size, 450 `.ae` files) has **56** externs total — almost entirely real C
bindings — and **1943 imports**. Its extern density is ~80× lower.

avn's heavy `extern` use isn't idiomatic; it's port-era ectoplasm. This
document lays out a methodical plan to migrate avn from `extern` cross-file
references to `import`-based modules, restoring escape analysis, simplifying
the build graph, and bringing avn in line with how Aether is intended to be
structured.

## Why this matters

Beyond aesthetic / "right thing":

1. **Escape analysis flows through `import` but not always through `extern`.**
   Aether's `function_def_returns_heap_at` and the structural escape walker
   need module-resolved visibility into the called function's body. Imports
   carry the module AST; externs are forward declarations the typer can't
   walk back through. The R288→R293 trail of "leak fix unmasked latent UAF
   in avn" issues we've been chasing all bottom out at this gap. See
   `further-bug-fix3.md` and the failed attempt at running the bench under
   aetherc 0.140.0.

2. **`@retain` and contract annotations propagate cleanly through imports.**
   With externs, an annotation on the declaration site has to be repeated at
   every consuming site, and the typer has no way to verify consistency.
   Imports turn this into a single source of truth.

3. **Build graph shrinks dramatically.** Today every `.build.ae` lists
   ~50-80 `regen("../<dir>/<file>.ae")` entries. With imports, those go
   away — the typer transitively pulls in every imported module's `.c`
   automatically.

4. **Rename / refactor / IDE tooling all work better.** "Find all callers
   of `io_write_atomic`" is currently a grep across `.ae` files for
   `extern io_write_atomic`. With imports, the typer answers it directly.

## Resolution rules (from aether's `docs/architecture.md`)

> Search paths: `std/<name>/module.ae` first, then `./<name>/module.ae`
> relative to the importer, then `contrib/<name>/module.ae`.

So `import util` literally finds `util/module.ae` *relative to the importer*.
The convention is one file per module, named `module.ae`, in a directory
named after the module. Multi-file modules don't exist as a first-class
construct.

For avn this means: **each existing `.ae`-fragment-laden directory becomes
ONE file** — `util/module.ae` containing the merged content of
`util/byte_ops.ae` + `util/int_vec.ae` + `util/io.ae` +
`util/remote_tree.ae`. We keep the flat name surface (`util.io_write_atomic`,
not `util.io.write_atomic`); matches aether stdlib's pattern.

## The dependency graph (from `extern` resolution counts)

```
from \ to     util  ffi  delta  client  repos  rstor  avserv  avadm  avn  wc
util            -    0    0      0       1     19      0      0     0    1
ffi             0    -    0      0       0      0      0      0     0    0
delta           0    0    -      0       0      0      0      0     0    0
client          0    0    0      -       0      1      0      0     0    0
repos           3    0    0     19       -     27      0      0     0    0
repo_storage   21    0    0      0      10      -      0      0     0    0
avnserver       8    0    0      0      36     29      -      0     0    0
avnadmin        2    0    0      0       2      4      0      -     0    0
avn             1    0    0     50       0      0      0      0     -   30
working_copy   27    0    0     25       0      1      0      0     0    -
```

Read row-wise: "directory X declares N externs that resolve to functions
in directory Y." Cycles and surprises:

- **`repos ↔ repo_storage` is a true bidirectional cycle** (27 in one
  direction, 10 in the other). Cannot be untangled without a structural
  decision: which side owns the boundary, and how do we split them.
- **`util` allegedly references `repo_storage` 19 times.** Almost certainly
  a matching artifact — both dirs export functions with similar names. Worth
  verifying before merging util/, but very unlikely to be a real cycle.
- **`client` is almost-leaf** (1 reference into `repo_storage`).
- **`avn` is almost-leaf at the top** — only consumes `client` and
  `working_copy`. `working_copy` is the next layer down.

## Migration order (least-depending-first, most-depended-on-first)

### Phase 0 — Validate mechanics (~5 min)

Create a one-function `proof/module.ae`:

```aether
export hello() -> string { return "hi from proof" }
```

Add `import proof` to one consumer (e.g. `avnserver/main.ae`). Build with
`aeb avnserver`.

**Acceptance:** binary builds, runs, `proof.hello()` returns the string.
**Stop condition:** if aetherc-via-aeb doesn't resolve project-local
`./proof/module.ae`, we need to figure out the search-path config (likely
`aether.toml`'s `[lib]` section) before any real migration. Don't proceed.

After validation, delete `proof/` and the import.

### Phase 1 — `ffi/` and `delta/` (true leaves, no incoming or outgoing avn refs)

Both are zero-row-zero-column in the matrix. Migrate independently — these
are confidence-builders. Each is one `module.ae` worth of merge.

**Round size:** small. Probably one commit per directory.

**Acceptance:** each binary that linked against `ffi/` or `delta/` still
builds; bench/tests stay green.

**Phase 1a status: DONE** in Round 293 — `ffi/openssl/crypto_helpers.ae`
→ `ffi/openssl/module.ae`, 9 consumers updated to `import ffi.openssl`.
Validated via `ae build` of `util/test_checksum.ae` (all sha1 checks
pass) and per-file `ae check` on all 10 modified files.

**Phase 1b status: DONE (Round 297).** Unblocked by aetherc 0.141.0
which fixed exactly this — see `aether/CHANGELOG.md` 0.141.0 entry
("Struct definitions in imported modules are now visible to that
module's own merged function bodies"). The bug report at
`../aether/exprt_structs.md` was filed and the fix landed within
hours; the same `Slot`/`as *Slot` reproducer that motivated the bug
report is the integration test in `aether/tests/integration/import_struct/`.

delta migration: 4 source files (`svndiff.ae` + `svndiff_decode.ae`
+ `svndiff_encode.ae` + `xdelta.ae` + `svndiff/encoder.ae`) merged
into `delta/module.ae`. The `Encoder` struct stays internal. Two
test files (`test_svndiff.ae`, `test_xdelta.ae`) switched from
`extern svndiff_*`/`extern xdelta_*` to `import delta` and prefixed
call sites with `delta.`. Both test suites green end-to-end:
- `test_svndiff` 6/6 cases (identity, pure new-data, src prefix+new,
  target self-copy RLE, word replace, varint length).
- `test_xdelta` 9/9 cases (identity, empty source, empty target,
  one-word swap, insertion, deletion, swap, large similar 2KB+tiny
  patch, no overlap).

### Phase 2 — `util/`

Merge `util/byte_ops.ae` + `util/int_vec.ae` + `util/io.ae` +
`util/remote_tree.ae` → `util/module.ae`. **Verify the alleged 19
references from util→repo_storage first** — if real, they're a cycle and
util/ isn't actually a leaf. Likely they're matching noise (function-name
collisions across dirs), in which case util/ is the cleanest first
real-codebase migration.

Consumers to update:
- `repos/` (3 externs → import util)
- `repo_storage/` (21 externs → import util)
- `avnserver/` (8 externs → import util)
- `avnadmin/` (2 externs → import util)
- `avn/` (1 extern → import util)
- `working_copy/` (27 externs → import util)
- (62 total extern declarations across 6 directories collapse into 6
  `import util` lines)

**Round size:** medium. ~80 file edits if we touch every consumer; only
~6 files added/deleted.

**Acceptance:** build green, server/test suite green, bench green.
Confirms the migration shape works against real code.

**Phase 2 status (Rounds 294-296):**

- **Phase 2a DONE (Round 294)**: byte_ops.ae merged into util/module.ae.
  Two consumers (working_copy/pristine.ae, repo_storage/rep_store.ae)
  switched to `import util`. Validated.

- **Phase 2b DONE (Round 295)**: int_vec.ae merged. Sole consumer
  util/remote_tree.ae switched to `import util` (its own struct Rtree
  stays local — Rtree blocks remote_tree.ae itself from migrating, but
  remote_tree consuming util's int_vec_* via import is fine).

- **Phase 2c (Round 296)**: io.ae merge. Pushed through despite an
  **aeb capability-propagation gap**. After merging io.ae into
  util/module.ae, util/module.ae imports std.fs. When consumer files
  switch from `extern io_*` to `import util`, they transitively pull
  std.fs via util. aeb's `_detect_caps` only scans the file's own
  `import std.X` lines and doesn't follow `import <module>`, so the
  regen pass calls `aetherc --emit=lib X.ae` without `--with=fs` and
  aetherc rejects: `Error: --emit=lib rejects 'import std.fs' without
  --with=fs.` Documented at `aeb/lib/aether/module.ae` line 254-259:
  "use `regen_with(path, "fs")` to declare caps explicitly."

  **Workaround applied**: every `regen("../X/Y.ae")` whose target Y.ae
  transitively pulls fs via util gets switched to
  `regen_with("../X/Y.ae", "fs")`. Touches every .build.ae /
  .tests-*.ae file but is mechanical.

  **Future aeb improvement** (to remove the workaround): extend
  `_detect_caps` to follow `import <mod>` in the scanned file, look
  up `<mod>/module.ae`'s caps, and union them in. One-time aeb change,
  fixes the issue for all future migrations and matches aeb's stated
  convention ("imports trigger automatic regen").

### Phase 3 — `client/`

`client/` has 14 source files (`accessors.ae`, `commit_build.ae`,
`commit_builder.ae`, `http_client.ae`, `parse.ae`, `verify.ae`, `urls.ae`,
`packed.ae`, …). Merge into `client/module.ae`. One outgoing
`client→repo_storage` extern needs to remain (or be inverted) — likely
`client` calls one repo-storage helper that should move sides; check on
inspection.

Consumers to update:
- `repos/` (19 externs)
- `avn/` (50 externs — the biggest single migration)
- `working_copy/` (25 externs)

**Round size:** larger. The avn-binary's 50 externs make this the heaviest
single round.

**Stop condition:** if the cycle with `repo_storage` (1 ref) doesn't
break cleanly, defer client/ to after the repos/repo_storage tangle is
sorted in Phase 4.

**Phase 3 status: DONE (Round 302).** Unblocked by aetherc 0.142.0's
selective-import propagation fix (Issue B from
`../aether/import_typer_at_scale.md`). 12 client/*.ae files merged
into client/module.ae. commit_builder.ae stays separate for now (its
struct RaCommit could now migrate under 0.141.0's fix; deferred to
keep this round focused). Namespace-collision workaround: selective
import `import std.http.client (set_header, send_request, ...)` —
works because consumers' compilation now sees the propagated bindings.

### Phase 4 — Untangle `repos` ↔ `repo_storage` cycle, then merge each

The 27-and-10 cycle is the hard one. Three options:

A. **Merge them into one module** (`repo_storage/module.ae` containing both
   subtrees of code). Simplest mechanically; loses the conceptual
   separation. Acceptable if the names are already disjoint.

B. **Split repos/ into two modules: `repos_core` (used by repo_storage) and
   `repos_handlers` (uses repo_storage)**. Restores acyclicity at the cost
   of one new module name.

C. **Inline the small side into the big side.** If only 10 references go
   `repo_storage → repos`, the 10 functions might naturally belong in
   repo_storage anyway. Move them.

Inspection tells us which is right. **Pick on inspection, not dogma.**

After untangling, migrate both as Phase 4a (`repos`) and Phase 4b
(`repo_storage`).

### Phase 5 — `working_copy/`

Depends on `client/`, `util/`, and one `repo_storage` reference. By Phase 5
all three are already imports, so `working_copy/` becomes a clean
import-only consumer. Merge into `working_copy/module.ae`.

Consumers to update:
- `avn/` (30 externs)

**Round size:** medium.

**Phase 5 status: DONE (Round 301).** Unblocked by aetherc 0.142.0's
**128-decl truncation cap fix** — the original Round 301 failures
weren't classical forward refs, they were the consumer-side typer
silently dropping decls past the 128th when the merged module had
~150 functions. With the cap lifted to 4096, 30 files merged
cleanly (~4300 lines). 7 file-private helpers were duplicated across
sibling files (record_start_, record_end_, list_field_, etc.) —
deduped in the merge. 84 functions had been both defined and
extern'd across files — 176 redundant extern declarations dropped.

### Phase 6 — Top binaries: `avnserver/`, `avnadmin/`, `avn/`

These are the program entry points. Each has its own `main.ae` plus a small
constellation of helper files (handlers, parsers, dispatchers in
`avnserver/`; subcommand handlers in `avn/`; admin operations in
`avnadmin/`).

Two sub-options for each binary:
- **Merge all `.ae` files in the binary's dir into one `module.ae`.** Largest
  diff but cleanest end-state.
- **Keep them as separate `.ae` files but switch from per-file linkage to a
  single-file program entry.** Less reorganization but doesn't fully resolve
  the extern issue.

Probably do the merge for symmetry with the lib directories.

For `avnserver/`, this is the largest single dir migration (40+ files —
every handler, parser, dispatcher). Could split into multiple rounds:
handlers first, parsers next, infrastructure last. Or do it in one round
and own the diff size.

### Phase 7 — Cleanup

- Strip every `regen("../<dir>/<file>.ae")` line from the three `.build.ae`
  files. Imports trigger automatic regen for module sources.
- Delete now-orphaned `*_generated.c` files (gitignored anyway, cleared on
  next clean build).
- Update `LLM.md` / `README.md` to describe the new module layout.

## Acceptance criteria — every phase

The same three checks pass after every phase:

1. `aeb avnserver && aeb avnadmin && aeb avn` build clean (no warnings
   beyond pre-existing).
2. `aeb avnserver/.tests-server.ae` runs the test suite — passing count
   must equal or exceed the pre-phase baseline (10 pre-existing failures
   may persist; new failures are a regression).
3. `python3 bench/bench_avn.py --no-compress` completes 5000 commits with
   RSS ≤ 5 GB (current healthy baseline ~3.3 GB; allowance for the
   un-fixed tuple-destructure leak that may be unblocked by improved
   escape analysis somewhere along the way).

If any of those regress, **stop and revert the phase** before proceeding.
The migration's value is in not-breaking-things; partial-and-broken is
worse than partial-and-working.

## Rollback / interruption

Each phase is one or two git commits. `git revert` cleanly backs any
phase out. Don't squash phases; the granularity is the safety net.

## Risks and unknowns

- **Module resolution config.** Phase 0 confirms whether avn needs an
  `aether.toml` `[lib]` setting to find project-local modules. If yes, the
  config is one round of work *before* Phase 1.
- **Merged-file size.** `avnserver/module.ae` post-merge could be 5000+
  lines. Aether's parser handles this fine in principle, but we should
  spot-check compile times after avnserver/. If they explode, switch to
  multi-module split for that directory.
- **Test driver files.** `.tests-*.ae` and `.test_*_driver.ae` reference
  binaries via fixtures, not the .ae layer. Their import surface is small
  — should sweep them in Phase 6 alongside the binary they test.
- **The currently-broken bench under aetherc 0.140.0.** As of writing, the
  bench segfaults at i=1 due to the missing escape-through-struct-field
  analysis (tracked in a planned `further-bug-fix5.md`). The migration
  *may* unblock the bench as a side effect — once `set_packed_` is reached
  via `import client_commit_builder`, the codegen's escape walker can see
  the `cb.field = s` write and propagate. If it doesn't, fall back to
  explicit `@retain` annotations on the merged module's exports.
- **Merge mechanics.** Concatenating files into `module.ae` requires
  hand-merging the imports at the top, deduping, and handling any
  file-private constants that shared a name across the original files
  (currently file-local; need to be uniqued in the merged module). Not
  hard but takes care.
- **`builder` keyword usage.** avn uses Aether's `builder` keyword in
  `avnserver/embed.ae`. Verify that `builder` declarations work cleanly
  inside an imported module's `module.ae`; nothing in the docs suggests
  they don't, but spot-check in Phase 0 or Phase 1.

## Estimated effort and ordering

| phase | scope | rough size | risk |
|------|-------|------------|------|
| 0 | proof/module.ae validation | trivial | mechanics-only |
| 1 | `ffi/` + `delta/` (leaves) | small | low |
| 2 | `util/` | medium (~80 sites updated) | low |
| 3 | `client/` | larger (~75 sites) | medium (cycle break) |
| 4a/b | `repos/` ↔ `repo_storage/` cycle untangle | largest by inspection | high |
| 5 | `working_copy/` | medium (~30 sites) | low |
| 6 | binaries: `avnserver`, `avnadmin`, `avn` | largest by file count | medium (40+ files in avnserver alone) |
| 7 | cleanup | small | none |

Total: 7 phases, 8-10 rounds, probably 1-2 weeks of methodical work spaced
across other priorities.

## Cross-references

- `408_TODO.md` — the structured-error / contracts / hardening / parse_strict
  fan-out. Some items there interact with the migration: e.g. fan-out of
  R288's structured-error pattern to the other handlers is easier to do
  *after* their host directory has been moduled, since shared KIND_*
  constants can live in the consolidated module rather than being
  duplicated per file.
- `bench/RESULTS.md` — current healthy baseline numbers.
- `further-bug-fix5.md` *(to be written)* — the aether-side escape-through-
  struct-field bug that motivates the escape-analysis story.
