# Notes to self (LLM assisting on avn)

Terse. Re-read at session start.

## What this repo is

A production-shape Subversion implementation in Aether. Not a port-
in-progress — the C-to-Aether translation is done, the upstream
reference tree was deleted in Round 204, and the code has been
through ~210 rounds of post-port naming, idiom, and architecture
work since then.

Three production binaries: `avn` (client CLI), `avnserver`
(HTTP server), `avnadmin` (repo admin). Plus `avn-seed` and a
small `test_client` used by the integration harness.

The on-disk repo format, wire protocol, dump format, ACL model, and
Merkle verification are all our own design — they share semantics
with reference Subversion (so users see familiar `avn` UX) but
diverge freely on plumbing where doing so paid off.

## Layout

- `<area>/module.ae` — first-party Aether source at the repo root,
  one directory per area: `client/`, `delta/`, `ffi/openssl/`,
  `repo_storage/`, `repos/`, `util/`, `working_copy/`. Each area is
  a single `module.ae` consumed via `import <area>` (R293-R308
  collapsed ~130 per-file fragments down to one file per area —
  see `methodical_extern_removal_plan.md`). Compiled to C via
  `aetherc --emit=lib`, linked into binaries by aeb's
  `regen_with(...)` setter. Generated `_generated.c` is gitignored.
- `<binary>/{main.ae,module.ae}` — top-of-graph binaries
  (`avn/`, `avnadmin/`, `avnserver/`). Binaries need `main()`, which
  modules can't carry, so each binary keeps its own `main.ae` and a
  sibling `module.ae` for everything else. `avnadmin/main.ae` does
  `import avnadmin`. `avnserver/main.ae` keeps link-layer externs
  to its own `module.ae`'s embed surface (transitive imports of
  std.* + util/ + repos/ + repo_storage/ + ffi.openssl exceed
  aetherc's 4096-decl cap when imported via `import avnserver`).
  `avn/` is just `main.ae` — no helpers to factor out.
- `aether.toml` — `[build]` section + a handful of legacy `[[bin]]`
  entries for unused Aether-native test programs (test_wc_db,
  test_repos, etc — built but not exercised by anything). The four
  production-binary entries (avn, avnadmin, avnserver,
  avn-seed) were retired in Round 219; each now lives in its own
  `.build.ae`.
- `tests/lib.sh` — bash helpers (`tlib_check`, `tlib_summary`,
  `tlib_stop_server`) used by the two surviving shell tests under
  `avnadmin/`. Everything else has migrated to `aether.driver_test`
  with a sibling `.test_*_driver.ae` driver.
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
  shipped SDKs (build, aether, bash, …). No port-local SDK any more
  — fixtures use `aether.driver_test`'s `fixture_seed`/`fixture_server`
  primitives directly.
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
aeb avn       # build the avn binary only
aeb .tests.ae    # full test suite, target mode
```

Outputs land under `target/<dir>/bin/<name>`.

### Test grammar

Each integration test has its own `.tests-<tag>.ae` declaring the
fixture via aeb's `aether.driver_test`. A sibling
`.test_<tag>_driver.ae` is the test body. The fixture exports
`${name}_PATH`, `${name}_BIN`, `${name}_PORT`, `${name}_PID`,
`${name}_LOG` for each fixture_seed/fixture_server; the driver
reads them via `os.getenv` and asserts via Aeocha matchers.

The two `avnadmin/test_*.sh` scripts are *meta-tests of avnadmin
itself* — their fixture *is* the test (repo creation, dump/load,
server lifecycle inline) — so they remain shell. They share three
helpers from `tests/lib.sh`: `tlib_check`, `tlib_summary`,
`tlib_stop_server`.

### Adding a new Aether-native unit test (Aeocha)

Untrodden in this repo — we haven't done this yet. Aeocha lives at
`~/scm/aether/contrib/aeocha/` and `import contrib.aeocha`
resolves via Aether's stdlib search path. The Aeocha README's
`example_self_test.ae` is the model: `aeocha.init()`, then
`describe`/`it` blocks with `assert_eq`/`assert_str_eq`.
Wire it via `aether.program_test(b)` in a `.tests-<X>.ae`.

### Migrating a bash integration test to Aether (`aether.driver_test`)

Aeb shipped `aether.driver_test(b)` with closure-form fixture
grammar. The shape replaces a `.sh` script body with an Aether
*driver program* that spawns the production binary as a child
process and asserts about stdout/exit/stderr via Aeocha's
integration-shape matchers (`expect_exit`, `expect_stdout_line_field`,
`expect_http_status`, ...).

Skeleton `.tests-X.ae`:

```aether
import build
import aether
import aether (driver, output, binary_under_test,
               fixture_seed, fixture_server,
               path, env_var, seed_bin, bin, args, port,
               ready_after_ms)

main() {
    b = build.start()
    build.dep(b, "avn")            // build the binary-under-test first
    build.dep(b, "svnserver")
    build.dep(b, "svnserver/.build-seed.ae")

    aether.driver_test(b) {
        driver(".test_X_driver.ae")
        output("X_driver")

        binary_under_test(b, "avn") {
            path("target/avn/bin/avn")
            // env_var defaults to $AVN_BIN
        }

        fixture_seed(b, "primary") {
            path("/tmp/svnae_test_X_repo")
            seed_bin("target/svnserver/bin/avn-seed")
        }
        fixture_server(b, "primary") {
            bin("target/svnserver/bin/avnserver")
            args("demo $PRIMARY_PATH 9540")
            port(9540)
            ready_after_ms(1500)
        }
    }
}
```

Skeleton `.test_X_driver.ae`:

```aether
import contrib.aeocha
import std.os

main() {
    fw = aeocha.init()
    avn_bin = os.getenv("AVN_BIN")
    port    = os.getenv("PRIMARY_PORT")
    url     = "http://127.0.0.1:${port}/demo"

    aeocha.describe(fw, "avn cli vs demo repo") {
        aeocha.it("info reports head rev 3") callback {
            argv = os.argv_new("info")
            os.argv_push(argv, url)
            r = os.run_capture(avn_bin, argv, null)
            aeocha.expect_exit(fw, r, 0, "avn exited 0")
            aeocha.expect_stdout_line_field(fw, r, "Revision:", 1, "3",
                "head rev")
        }
    }
    aeocha.run_summary(fw)
}
```

Required vs optional sub-setters:

- `fixture_seed`: `path` required, `seed_bin` optional (omit → just `mkdir -p`).
- `fixture_server`: `bin` and `port` required, `args` defaults to "",
  `ready_after_ms` defaults to 0.
- `binary_under_test`: `path` required, `env_var` defaults to `<UPPER(name)>_BIN`.

Env-var contract exposed to the driver:
`$<NAME>_PATH`, `$<NAME>_PORT`, `$<NAME>_PID`, `$<NAME>_BIN`.

Lifecycle: per-script seeds-then-servers spawn → sleep
`ready_after_ms` → run driver → kill servers in reverse order →
rm fixture paths. Sequential test mode is forced when fixtures
are present.

`fixture_server`'s `args` string shell-interpolates at run time, so
`args("demo $PRIMARY_PATH 9540")` picks up the seed's exported path.

Two stragglers (`test_hash_algo`, `test_avnadmin`) are meta-tests
of avnadmin itself — their fixture *is* the test, so they manage
repo creation and server lifecycle inline. Don't migrate them.

#### Driver-side gotchas (Round 234 canary findings)

- **Raw externs need named imports.** `import std.os` exposes
  *wrapper* functions like `os.run_capture` but NOT raw externs.
  For `os_getenv`, `os_run`, `list_new`, `list_add_raw`, etc, you
  need `import std.os (os_getenv)` and call them unqualified.
  Compile error reads "Undefined function 'os.os_getenv'."
- **Aeocha install path.** `import contrib.aeocha` resolves via
  `/usr/local/share/aether/contrib/aeocha/` (the system PREFIX,
  not `$HOME/.local/share`). The Aether build's `install-contrib`
  target trims aeocha from the user-prefix install (Makefile line
  ~1155) but the system install includes it. If `import
  contrib.aeocha` doesn't resolve, check
  `ls /usr/local/share/aether/contrib/aeocha/module.ae`.
- **`extern list_new` collides with aeocha's import.** Aeocha
  declares `extern list_new` / `extern list_add_raw` in its module.
  When a driver also declares them at module scope, aetherc emits
  conflicting prototypes (`void* list_new()` from aeocha after the
  closures vs implicit-int from your closure bodies). Workaround:
  declare the externs at module scope BUT call them only through a
  thin Aether wrapper:
  ```aether
  extern list_new() -> ptr
  extern list_add_raw(list: ptr, item: ptr) -> int
  argv_new() -> { return list_new() }
  argv_push(argv: ptr, s: string) { list_add_raw(argv, s) }
  ```
  Then `argv = argv_new()` / `argv_push(argv, "info")` in closures.
  This keeps the implicit-int issue out of closure bodies.
  **Same pattern bites `string.concat`, `client.request`,
  `client.set_timeout`, `client.send_request`, `client.request_free`,
  `client.response_body`, `client.response_free`** — anything
  called inside a closure body whose extern decl ships in an
  imported module. Round 235 HTTP canary uses `http_get(url)`,
  `resp_free(resp)`, `resp_body(resp)` wrappers; pre-computes
  URL strings outside closures via
  `url_r4 = string.concat(paths_base, "4/paths")` to dodge the
  `string.concat`-in-closure variant. Upstream ask:
  `~/scm/aether/closure-extern-ordering.md`.
- **No `os.chdir` in std.** Cwd-bound commands (`avn add NEW`
  inside a WC) need `/bin/sh -c "cd $WC && avn ..."`. Driver
  pattern:
  ```aether
  sh(cmd: string) -> {
      argv = argv_new()
      argv_push(argv, "-c"); argv_push(argv, cmd)
      out, status, err = os.run_capture("/bin/sh", argv, null)
      return out, status, err
  }
  ```
  Inline shell strings in the driver are fine for the
  per-test prep step. WC-prep happens before the `describe` block
  so prep failures surface as the first assertion in the suite.
- **Multi-fixture pattern.** Tests needing N>1 servers (e.g.
  `test_switch` switching between two repos) declare each
  fixture explicitly:
  ```aether
  fixture_seed(b, "primary")   { path("..."); seed_bin("...") }
  fixture_seed(b, "secondary") { path("..."); seed_bin("...") }
  fixture_server(b, "primary")   { bin("..."); args("demo $PRIMARY_PATH 9480"); port(9480); ... }
  fixture_server(b, "secondary") { bin("..."); args("demo $SECONDARY_PATH 9481"); port(9481); ... }
  ```
  Each fixture's name uppercases to a prefix: `$PRIMARY_PORT`,
  `$PRIMARY_PATH`, `$SECONDARY_PORT`, etc. aeb spawns/kills both
  per test invocation. Round 236 canary:
  `working_copy/test_switch_driver.ae`.
- **Token-auth pattern.** Inline the token in `args`:
  ```aether
  fixture_server(b, "test_acl") {
      bin("target/svnserver/bin/avnserver")
      args("demo $TEST_ACL_PATH 9540 --superuser-token test-super-token-42")
      port(9540); ready_after_ms(1500)
  }
  ```
  Round 236 canary: `avn/test_acl_driver.ae`.
- **Per-call env vars (SVN_USER=alice etc).** No `os.run_capture`
  env-list builder helper yet. Use `/bin/sh -c` and prefix the
  shell variables: `sh("SVN_USER=alice ${avn_bin} ls ${url}")`.
  Composes with the `sh()` helper above.
- **HTTP with custom headers.** `http_get_with_user(url, "alice")`
  / `http_get_with_super(url, token)` wrappers around
  `client.request → set_header → set_timeout → send_request →
  request_free`. Same pattern as `http_get` but with a
  `client.set_header(req, "X-Svnae-User", value)` step inserted
  between request creation and send. See ACL driver.
- **String concat helper hygiene.** Deeply nested
  `string.concat(a, string.concat(b, ...))` becomes unreadable past
  ~6 levels and easy to mis-balance parens. Pattern: build commands
  step-by-step via `cmd = string.concat(cmd, "...")` re-assignment
  in `main()`, then reference the resulting `cmd` string from the
  closure. Same approach for URLs (`url_r4 = ...; url_r5 = ...`).
- **`binary_under_test` and `fixture_server` env_var name collision.**
  When the binary-under-test name matches the fixture name, both
  default to `$<UPPER>_BIN`. The fixture_server's export wins
  (overwrites the binary_under_test export); the driver then
  spawns the server binary thinking it's the under-test. Symptom:
  aeb hangs forever after build because the spawned process is
  the wrong binary. Fix: explicit `env_var("X_DRIVER_BIN")` on
  the binary_under_test block. Round 237 finding (canary:
  `client/.tests.ae`).
- **`/bin/sh` vs `/bin/bash`.** `/bin/sh` is dash on Linux and
  doesn't support `$'...'` ANSI-C escapes. If you need newlines
  inside a string passed to a tool (e.g. multi-pattern svn:ignore
  values), invoke `/bin/bash -c` instead of `/bin/sh -c`. The
  `sh()` wrapper in our drivers should target bash for that
  reason. Round 237 finding.
- **`-lnghttp2` link flag.** The Aether stdlib's HTTP/2 surface
  pulls libnghttp2 into `libaether.a`, so any binary linking
  `libaether.a` needs `link_flag("-lnghttp2")`. Symptom: `gcc link
  failed` with a long list of `undefined reference to
  nghttp2_*`. Round 238 svnserver build hit this.
- **Tuple-destructure shadow collision.** If main() destructures
  a wrapper's tuple return `(out, status, err) = sh(...)` AND a
  closure body destructures with overlapping names (`out, status,
  _ = sh(...)`), aetherc miscompiles the closure-local slot and
  the binary segfaults at startup with no output. Workaround:
  rename main()'s outer destructure to non-overlapping names
  (e.g. `prep_out_`, `prep_st_`, `prep_err_`) so closure-local
  `out`/`status` shadows don't collide. Symptom: no test output,
  `signal 11 segmentation fault (core dumped)` in the driver run
  before the `describe` block prints. Round 238 commit driver
  blocked on this for a session; minimal repro at
  `~/scm/aether/closure-shadow-tuple-destructure.md`.
- **`ae cache clear`** when in doubt. aetherc's content-addressed
  cache at `~/.aether/cache/` doesn't always invalidate when an
  imported module changes transitively. If you see "Built (cache
  hit)" but suspect old code is being run, `ae cache clear`
  forces a re-emit.
- **Fixture authoring is inline now.** No port-local SDK; write
  `fixture_seed(b, NAME) { path(...); seed_bin(...) }` and
  `fixture_server(b, NAME) { bin(...); args(...); port(...) }`
  directly in `.tests-<tag>.ae`.

## What stays in C

Nothing in our tree. The SQLite veneer (`aether_sqlite.c`) is
provided by Aether's prebuilt `libaether_sqlite.a` at
`$PREFIX/lib/aether/`; we link it via `link_flag("-laether_sqlite")`
in each `.build.ae`. Our own `.ae` source compiles to C via
`aetherc`, but that C is generated and gitignored.

## Aether specifics that bite

1. **`--emit=lib` requires `--with=net|fs|os` for capability-touching
   imports.** aeb's `regen(...)` setter auto-detects from
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
aeb avn                 # build just the avn CLI

# Run one bash test directly (interactively):
bash avn/test_acl.sh    # if its fixture is satisfied

# Run all tests for one directory:
aeb avn                 # via the dir's .tests.ae (or first .tests-*.ae)
```

## When stuck

- `git log --oneline -30` — recent baby commits.
- `TODO.md` — outstanding small work.
- `~/scm/aether/LLM.md` for Aether-side conventions.
- `~/scm/aetherBuild/README.md` for aeb DSL reference.

## Don't

- Don't reintroduce `regen.sh` or per-directory `Makefile.regen`.
  aeb's `aether.program(b) { regen(...) }` setter handles codegen.
- Don't add `[[bin]]` blocks to `aether.toml`. Each binary owns its
  own `.build.ae`.
- Don't put server spawn/kill code in test bodies. Use
  `fixture_server(b, NAME) { ... }` inside `aether.driver_test`.
- Don't batch into mega-commits. Baby commits, tests green between
  each.
