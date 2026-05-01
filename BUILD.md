# SVN-Aether Build & Test System

This document describes the unified test runner and build grammar for the svn-aether port.

## Test Discovery & Running

All integration tests live under `ae/*/test_*.sh`. The test runner discovers them automatically.

```bash
# Run all 32 tests in parallel
./tests/run.sh

# Run tests matching a pattern
./tests/run.sh test_svn          # runs ae/svn/test_*.sh
./tests/run.sh test_commit       # runs tests with "commit" in name

# Control parallelism
./tests/run.sh --jobs 4          # 4 parallel jobs
./tests/run.sh --jobs 1          # serial (debug)

# Override the Aether binary
AETHER=/path/to/ae ./tests/run.sh
```

Exit code: 0 if all tests pass, 1 if any fail.

Output format (aetherBuild-style):
```
svn-aether test suite
repo: /home/user/avn
ae:   /path/to/ae
jobs: 16

running 32 integration tests...

  test_svn                         ✓ok
  test_commit                      ✓ok
  test_merge                       ✗FAIL

---
PASS: 31 / 32   FAIL: 1   (jobs=16)

failed tests:
  test_merge
```

## Integration Test Grammar (Future)

The current 32 bash tests can be gradually migrated to a declarative grammar. Proposed syntax (inspired by aetherBuild):

### Simple test_*.sh files (current)

Keep the bash integration tests as-is. They're discoverable and runnable by the unified runner.

### Future: Declarative test suites with `.tests.ae`

For complex multi-binary tests, future `.tests.ae` files in `ae/*/` could declare:

```aether
// ae/svn/.tests.ae — declarative test suite
import build
import test

main() {
    // Define test suite
    suite = test.suite("svn client operations")
    
    // Declare what to build
    suite.build("ae/svnserver/main.ae", "svnserver")
    suite.build("ae/svnserver/seed.ae",  "seeder")
    suite.build("ae/svn/main.ae",        "svn")
    
    // Declare test cases
    suite.case("checkout", {
        setup: fn() { seeder.run("/tmp/test_repo") },
        test: fn() { svn.run("checkout http://localhost:9999/demo") }
    })
    
    suite.case("commit", {
        test: fn() {
            svn.run("edit README")
            svn.run("commit --author alice --log 'test'")
        }
    })
    
    // Run all cases
    test.run(suite)
}
```

This is a future extension. The bash tests remain the primary mechanism for now.

## Test Structure

Each `test_*.sh` follows a standard pattern:

1. **Setup**: Build binaries, create temp repo
2. **Execute**: Run server, execute CLI commands
3. **Verify**: Check output, compare expected vs actual
4. **Cleanup**: Kill server, remove temp files (trap)

Example:

```bash
#!/bin/bash
set -e
cd "$(dirname "$0")/../.."

# Setup
./regen.sh >/dev/null
ae build ae/svnserver/main.ae -o /tmp/server
ae build ae/svn/main.ae -o /tmp/svn
REPO=/tmp/test_repo_$$
mkdir -p "$REPO"

# Cleanup trap
trap 'pkill -f /tmp/server; rm -rf "$REPO"' EXIT

# Execute
/tmp/server /srv/repo 9999 &
sleep 1

# Verify
result=$(/tmp/svn checkout http://localhost:9999/demo | grep "Checked out")
[ -n "$result" ] || exit 1

echo "✓ test passed"
```

## Build System

- **aether.toml** — Project configuration (binaries, link flags)
- **build.sh** — Orchestrator: `./regen.sh` then `ae build` each binary
- **sync-aether-deps.sh** — Download Aether toolchain
- **regen.sh** — Regenerate `_generated.c` files from Aether sources

Tests invoke `./regen.sh` internally to ensure generated code is fresh.

## Continuous Integration

The runner is CI-friendly:

```bash
# GitHub Actions example
- run: ./tests/run.sh --jobs 2
```

Output is machine-parseable (PASS/FAIL per line) for CI dashboards.

## Notes

- Tests use isolated temp directories (`/tmp/svnae_test_<tag>_*`) so parallel runs don't interfere.
- Each test picks its own `PORT` (e.g., 9350 for client, 9351 for server) to avoid conflicts.
- Tests are fast (~2-5 sec each) so 32 tests in parallel completes in ~10 sec.
- All tests clean up after themselves via `trap` handlers.
