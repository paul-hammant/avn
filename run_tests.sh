#!/bin/bash
# Parallel sweep of every test_*.sh under ae/. Each test uses its own
# REPO path (/tmp/svnae_test_<tag>_repo) and PORT, so they're safe to
# run concurrently.
#
# Usage:
#   ./run_tests.sh               # auto: nproc / 2 jobs
#   ./run_tests.sh 4             # explicit job count
#   ./run_tests.sh 1             # serial (debug; matches old loop)
#
# Output: one "ok <path>" or "FAIL <path>" line per test plus a final
# pass/fail summary. Exit 0 on all-green, 1 if anything failed.

set -e
cd "$(dirname "$0")"

JOBS="${1:-$(( $(nproc) / 2 ))}"
[ "$JOBS" -lt 1 ] && JOBS=1

mapfile -t tests < <(find ae -name "test_*.sh" | sort)

# xargs -P + -I{} together: -I disables -n; pass tests one-per-line.
results=$(printf '%s\n' "${tests[@]}" | \
    xargs -P "$JOBS" -I{} bash -c 'bash "{}" >/dev/null 2>&1 && echo "ok {}" || echo "FAIL {}"')

echo "$results" | sort

pass=$(echo "$results" | grep -c "^ok " || true)
fail=$(echo "$results" | grep -c "^FAIL " || true)

echo "---"
echo "PASS: $pass / ${#tests[@]}   FAIL: $fail   (jobs=$JOBS)"

[ "$fail" -eq 0 ]
