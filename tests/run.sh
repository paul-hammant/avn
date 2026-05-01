#!/usr/bin/env bash
# tests/run.sh — unified test runner for svn-aether
#
# Discovers and runs all tests in ae/*/test_*.sh (integration tests) and
# test_*.ae (unit tests). Modeled on aetherBuild's runner, extended for
# integration test suites. Tests run in parallel with independent repos/ports.
#
# Usage:
#   ./tests/run.sh                    # run all tests
#   ./tests/run.sh test_svn           # run tests matching pattern
#   ./tests/run.sh --jobs 2           # override parallelism
#   AETHER=/path/to/ae ./tests/run.sh # override ae binary
#
# Exit codes:
#   0  all tests passed
#   1  one or more tests failed to build or run

set -u

# Locate the script directory portably
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

AETHER="${AETHER:-ae}"
PATTERN="${1:-}"
JOBS="${JOBS:-$(( $(nproc 2>/dev/null || echo 2) / 2 ))}"
[ "$JOBS" -lt 1 ] && JOBS=1

# Parse explicit --jobs flag
if [ "$PATTERN" = "--jobs" ] && [ -n "${2:-}" ]; then
    JOBS="$2"
    PATTERN="${3:-}"
fi

if ! command -v "$AETHER" >/dev/null 2>&1; then
    echo "error: '$AETHER' not found in PATH. Set AETHER=/path/to/ae or install it." >&2
    exit 1
fi

TMPDIR_RUN="$(mktemp -d 2>/dev/null || mktemp -d -t 'svnae_tests')"
trap 'rm -rf "$TMPDIR_RUN"' EXIT INT TERM

# Counters
total=0
passed=0
failed=0
failed_list=""

# ANSI colors (only if stdout is a tty)
if [ -t 1 ]; then
    C_GREEN="$(printf '\033[32m')"
    C_RED="$(printf '\033[31m')"
    C_YELLOW="$(printf '\033[33m')"
    C_DIM="$(printf '\033[2m')"
    C_RESET="$(printf '\033[0m')"
else
    C_GREEN=""
    C_RED=""
    C_YELLOW=""
    C_DIM=""
    C_RESET=""
fi

pad_name() {
    name="$1"
    printf '%-32s' "$name"
}

# Discover bash integration tests (test_*.sh)
echo
echo "svn-aether test suite"
echo "repo: $REPO_ROOT"
echo "ae:   $(command -v "$AETHER")"
echo "jobs: $JOBS"
echo

# Find all test_*.sh files
bash_tests="$(find "$REPO_ROOT/ae" -name "test_*.sh" -type f | sort)"

if [ -z "$bash_tests" ]; then
    echo "no integration tests (test_*.sh) found"
    exit 0
fi

# Convert to array for xargs processing
mapfile -t test_array < <(echo "$bash_tests")

echo "running ${#test_array[@]} integration tests..."
echo

# Run tests in parallel using xargs pattern from aetherBuild
results=$(
    printf '%s\n' "${test_array[@]}" | \
    xargs -P "$JOBS" -I{} bash -c '
        test_path="{}"
        test_name="$(basename "$test_path" .sh)"

        # Pattern filter
        if [ -n "$PATTERN" ] && ! echo "$test_name" | grep -q "$PATTERN"; then
            exit 0
        fi

        printf "  %s " "$(printf "%-32s" "$test_name")"

        if bash "$test_path" >/dev/null 2>&1; then
            printf "%sok%s\n" "$C_GREEN" "$C_RESET"
            echo "PASS:$test_path"
        else
            printf "%sFAIL%s\n" "$C_RED" "$C_RESET"
            echo "FAIL:$test_path"
        fi
    ' BASH_VERSION="$BASH_VERSION" C_GREEN="$C_GREEN" C_RED="$C_RED" C_RESET="$C_RESET" PATTERN="$PATTERN"
)

# Parse results
pass=0
fail=0
while IFS=: read -r status path; do
    if [ "$status" = "PASS" ]; then
        pass=$((pass + 1))
    elif [ "$status" = "FAIL" ]; then
        fail=$((fail + 1))
        failed_list="${failed_list}$(basename "$path")
"
    fi
done <<< "$results"

echo
echo "---"
echo "PASS: $pass / ${#test_array[@]}   FAIL: $fail   (jobs=$JOBS)"

if [ "$fail" -gt 0 ]; then
    echo
    echo "failed tests:"
    echo "$failed_list" | sed 's/^/  /'
    exit 1
fi

exit 0
