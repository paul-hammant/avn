#!/bin/bash
# run_tests.sh — delegate to unified test runner
#
# Maintained for backward compatibility. Delegates to tests/run.sh which
# provides aetherBuild-style test discovery and execution.
#
# Usage:
#   ./run_tests.sh               # auto: nproc / 2 jobs
#   ./run_tests.sh 4             # explicit job count
#   ./run_tests.sh 1             # serial (debug)
#   ./run_tests.sh test_svn      # pattern match
#
# See BUILD.md for details.

set -e
cd "$(dirname "$0")"

# Parse arguments: first arg could be jobs count or pattern
first_arg="${1:-}"

if [ -n "$first_arg" ] && [ "$first_arg" -eq "$first_arg" ] 2>/dev/null; then
    # It's a number (job count)
    exec ./tests/run.sh --jobs "$first_arg" "${2:-}"
else
    # It's a pattern or empty
    exec ./tests/run.sh "$first_arg"
fi
