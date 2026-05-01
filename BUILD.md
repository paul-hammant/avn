# SVN-Aether Build & Test System

This document describes the Aether-based test framework for the svn-aether port.

## Test Declaration (Aether)

All 32 integration tests are declared in `.tests.ae` files using the `integration_test` framework.

### Root test suite (`.tests.ae`)

Run all 32 tests:

```bash
# Build the master test suite
ae build .tests.ae -o /tmp/run_all_tests

# Execute
/tmp/run_all_tests
```

### Module-level test suites

Each module with tests declares its suite:

```bash
# Client tests only
ae build ae/svn/.tests.ae -o /tmp/test_svn
/tmp/test_svn

# Working copy tests
ae build ae/wc/.tests.ae -o /tmp/test_wc
/tmp/test_wc

# Server tests
ae build ae/svnserver/.tests.ae -o /tmp/test_server
/tmp/test_server

# Admin tests
ae build ae/svnadmin/.tests.ae -o /tmp/test_admin
/tmp/test_admin
```

### Test declaration grammar

Each `.tests.ae` file uses the `integration_test` library:

```aether
import subr.integration_test (
    test_suite_new,
    test_suite_add,
    test_suite_set_jobs,
    test_suite_run_and_exit
)

main() {
    suite = test_suite_new("client operations")
    
    // Add bash test scripts by path
    test_suite_add(suite, "ae/svn/test_svn.sh")
    test_suite_add(suite, "ae/svn/test_commit.sh")
    test_suite_add(suite, "ae/svn/test_merge.sh")
    
    // Control parallelism
    test_suite_set_jobs(suite, 4)
    
    // Run and exit with status code
    test_suite_run_and_exit(suite)
}
```

## Bash Integration Tests

The 32 bash test scripts in `ae/*/test_*.sh` remain unchanged. They're orchestrated declaratively via `.tests.ae` files.

```bash
# Individual test (for debugging)
bash ae/svn/test_svn.sh

# View output
bash ae/svn/test_svn.sh 2>&1 | head -50
```

Each test:
- Builds its own binaries (svnserver, svn CLI, etc.)
- Creates isolated `/tmp/svnae_test_*` repos
- Picks unique ports (9350+) to avoid conflicts
- Cleans up via `trap` handlers

## Legacy Runner (Backward Compat)

For convenience, `run_tests.sh` and `tests/run.sh` still work:

```bash
# Old bash runner (delegates to tests/run.sh)
./run_tests.sh              # run all 32 tests
./run_tests.sh 4            # 4 parallel jobs
./run_tests.sh test_svn     # pattern match
```

This is now a thin bash wrapper that discovers and runs tests.

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
