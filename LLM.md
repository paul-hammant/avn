# Notes to self (LLM assisting on svn-aether)

Terse. Re-read at session start.

## What this repo is

A production-shape Subversion implementation in Aether. Not a port-
in-progress — the C-to-Aether translation is done, the upstream
reference tree was deleted in Round 204, and the code has been
through ~210 rounds of post-port naming, idiom, and architecture
work since then.

Three production binaries: `svn` (client CLI), `aether-svnserver`
(HTTP server), `svnadmin` (repo admin). Plus `svnae-seed` and a
small `test_client` used by the integration harness.

The on-disk repo format, wire protocol, dump format, ACL model, and
Merkle verification are all our own design — they share semantics
with reference Subversion (so users see familiar `svn` UX) but
diverge freely on plumbing where doing so paid off.

## Layout

- `<area>/*.ae` — first-party Aether source at the repo root (one
  directory per area: `client/`, `delta/`, `repo_storage/`,
  `working_copy/`, etc). Compiled to C via `aetherc --emit=lib`,
  linked into binaries by aeb's `regen(...)` setter. Generated
  `_generated.c` is gitignored.
- `aether.toml` — `[build]` section + a handful of legacy `[[bin]]`
  entries for unused Aether-native test programs (test_wc_db,
  test_repos, etc — built but not exercised by anything). The four
  production-binary entries (svn, svnadmin, aether-svnserver,
  svnae-seed) were retired in Round 219; each now lives in its own
  `.build.ae`.
- `.aeb/lib/svnae/module.ae` — port-local aeb SDK. Exposes
  `svn_server`, `svn_server_with_token`, `empty_repo`, `empty_server`,
  `empty_repo_with_algos`, `empty_server_with_algos` setters that
  generate the right pre/post_command pairs for bash test fixtures.
- `tests/lib.sh` — bash glue under the swan: `tlib_check`,
  `tlib_summary`, plus `tlib_seed_named` / `tlib_fixture_server` /
  `tlib_kill_servers` that the SDK setters call from pre/post_command.
- `.tests.ae` (repo root) — aggregator. `build.dep`s every leaf
  `.tests*.ae`. Run `aeb` for scan-mode discovery or `aeb .tests.ae`
  for target-mode equivalent.
- `PORT_STATUS.md` — historical narrative through the port. Stale
  after Round 31 but kept for context.
- `TODO.md` — outstanding work (small).

## Companion repos

- **Aether language**: `~/scm/aether/`. Toolchain (`ae`, `aetherc`)
  installed at `~/.local/bin/`. New language features land
  upstream; check `~/scm/aether/CHANGELOG.md` `[current]` when
  Paul mentions one.
- **aetherBuild**: `~/scm/aetherBuild/`. Provides `aeb`, the build
  driver. `aeb --init` creates the `.aeb/lib/*` symlinks for the
  shipped SDKs (build, aether, bash, …). The port-local svnae SDK
  is tracked in-tree; it lives at `.aeb/lib/svnae/` (gitignore is
  set up to exclude the symlinked SDKs but track `svnae`).
- **Aeocha** (Aether-native test framework): installed by Aether to
  `~/.local/share/aether/contrib/aeocha/`. Not used for the existing
  32 integration tests (those are bash scripts driving compiled
  binaries end-to-end), but the natural fit for new pure-Aether
  unit tests if/when we add them. `import contrib.aeocha` resolves
  via Aether's contrib search path.

## Build & test

One command:

```
aeb              # build every binary, run all 32 integration tests
```

`aeb .tests.ae` does the same in target mode.

Per-directory targets work too:

```
aeb svn       # build the svn binary only
aeb .tests.ae    # full test suite, target mode
```

Outputs land under `target/<dir>/bin/<name>`.

### Test grammar

Each integration test has its own `.tests-<tag>.ae` declaring the
fixture via the svnae SDK:

```aether
import build
import bash
import bash (script, pre_command, post_command)
import svnae
import svnae (svn_server)

main() {
    b = build.start()
    build.dep(b, "svnserver")
    build.dep(b, "svn")
    bash.test(b) {
        svn_server("acl", "9540")
        script("test_acl.sh")
    }
}
```

The fixture spawns a seeded server, exports `${name}_PORT` and
`${name}_REPO` into the script's env, kills the server after.
Test scripts contain assertion logic only, no spawn/kill code.

30 of 32 tests use this pattern. The 2 stragglers (`test_hash_algo`,
`test_svnadmin`) are *meta-tests of svnadmin itself* — their fixture
*is* the test, so they manage repo creation and server lifecycle
inline. Don't migrate them.

### Adding a new bash integration test

1. Write `<area>/test_X.sh` with `source "$(dirname "$0")/../tests/lib.sh"` then assertions calling `tlib_check`. End with `tlib_summary "test_X"`.
2. Add `<area>/.tests-X.ae` declaring the fixture.
3. Add a `build.dep(b, "<area>/.tests-X.ae")` line to the root `.tests.ae` aggregator.
4. Run `aeb` — should pass.

### Adding a new Aether-native unit test (Aeocha)

Untrodden — we haven't done this yet. Aeocha is installed at
`~/.local/share/aether/contrib/aeocha/` and `import contrib.aeocha`
resolves via Aether's stdlib search path. The Aeocha README's
`example_self_test.ae` is the model: `aeocha.init()`, then
`describe`/`it` blocks with `assert_eq`/`assert_str_eq`.
Wire it via `aether.program_test(b)` in a `.tests-<X>.ae`.

## What stays in C

Nothing in our tree. The SQLite veneer (`aether_sqlite.c`) is
provided by Aether's prebuilt `libaether_sqlite.a` at
`$PREFIX/lib/aether/`; we link it via `link_flag("-laether_sqlite")`
in each `.build.ae`. Our own `.ae` source compiles to C via
`aetherc`, but that C is generated and gitignored.

## Aether specifics that bite

1. **`--emit=lib` requires `--with=net|fs|os` for capability-touching
   imports.** The svnae SDK's `regen(...)` setter auto-detects from
   `import std.<X>` lines. Use `regen_with(NAME, "net,fs")` to override
   when imports are transitive.
2. **Setters are fixed-arity.** `extra_source("a", "b", "c")` won't
   compile — Aether disallows variadics. Repeat the call: one per
   item.
3. **Bare-name resolution inside `receiver.method(b) { block }`**:
   the block doesn't auto-resolve identifiers against `receiver`. You
   need an explicit named import: `import bash (script, jobs)`. See
   `~/scm/aether/dsl-block-receiver-scoping.md` for the language ask.
4. **String interpolation eats `${...}`** at parse time. To pass a
   literal `${var}` through to a shell command, use `$var` (no
   braces) or build the string with `string.concat`.
5. **Reserved keywords**: `state`, `match`, `message`. Rename to
   `st`, `is_match`, `msg`.

## Daily-driver workflow

```
# from /home/paul/scm/subversion/subversion
aeb                        # build + run 32 tests
aeb .tests.ae              # same, target mode
aeb svn                 # build just the svn CLI

# Run one bash test directly (interactively):
bash svn/test_acl.sh    # if its fixture is satisfied

# Run all tests for one directory:
aeb svn                 # via the dir's .tests.ae (or first .tests-*.ae)
```

## When stuck

- `git log --oneline -30` — recent baby commits.
- `TODO.md` — outstanding small work.
- `~/scm/aether/LLM.md` for Aether-side conventions.
- `~/scm/aetherBuild/README.md` for aeb DSL reference.
- `.aeb/lib/svnae/module.ae` is short — read it directly when
  designing a new fixture setter.

## Don't

- Don't reintroduce `regen.sh` or per-directory `Makefile.regen`.
  aeb's `aether.program(b) { regen(...) }` setter handles codegen.
- Don't add `[[bin]]` blocks to `aether.toml`. Each binary owns its
  own `.build.ae`.
- Don't put server spawn/kill code in test bodies. Use the svnae SDK
  fixture setters (or extend the SDK if a new shape is needed).
- Don't batch into mega-commits. Baby commits, tests green between
  each.
