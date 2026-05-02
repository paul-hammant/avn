# Shared bash test helpers. Sourced by ae/*/test_*.sh.
#
# After sourcing, $ROOT is repo root and these are pre-set with
# canonical default paths to the four port binaries (env-overridable):
#
#   $ADMIN_BIN   target/ae/svnadmin/bin/svnadmin
#   $SERVER_BIN  target/ae/svnserver/bin/aether-svnserver
#   $SEED_BIN    target/ae/svnserver/bin/svnae-seed
#   $SVN_BIN     target/ae/svn/bin/svn
#
# Tests get tlib_check + tlib_summary for assertion + final reporting.
# Tests still own port/repo/trap setup — those vary too much per test
# to live here usefully.

set -e
cd "$(dirname "${BASH_SOURCE[1]}")/../.."
ROOT="$(pwd)"

ADMIN_BIN="${ADMIN_BIN:-$ROOT/target/ae/svnadmin/bin/svnadmin}"
SERVER_BIN="${SERVER_BIN:-$ROOT/target/ae/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/ae/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/ae/svn/bin/svn}"

FAILS=0
tlib_check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  ok   $label"
    else
        echo "  FAIL $label"
        echo "       expected: $(echo "$expected" | head -c 200)"
        echo "       got     : $(echo "$actual" | head -c 200)"
        FAILS=$((FAILS + 1))
    fi
}

tlib_summary() {
    if [ "$FAILS" -gt 0 ]; then
        echo ""
        echo "FAIL: $FAILS case(s)"
        exit 1
    fi
    echo ""
    echo "${1:-test}: OK"
}
