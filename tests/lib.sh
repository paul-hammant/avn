# Shared bash helpers for the surviving shell tests under svnadmin/.
#
# All other test areas have migrated to aether.driver_test (Aether
# drivers under .test_*_driver.ae). This file keeps just what those
# two .sh scripts still need:
#
#   tlib_check LABEL EXPECTED ACTUAL
#   tlib_summary "test_name"
#   tlib_stop_server                  # kill $SRV; wait for it
#
# After sourcing, $ROOT is the repo root and the four binary paths
# below are pre-set (env-overridable):
#
#   $ADMIN_BIN   target/svnadmin/bin/svnadmin
#   $SERVER_BIN  target/svnserver/bin/aether-svnserver
#   $SEED_BIN    target/svnserver/bin/svnae-seed
#   $SVN_BIN     target/svn/bin/svn

if [ -n "${BASH_SOURCE[1]:-}" ] && [ -f "${BASH_SOURCE[1]}" ]; then
    cd "$(dirname "${BASH_SOURCE[1]}")/.."
    set -e
fi
ROOT="$(pwd)"

ADMIN_BIN="${ADMIN_BIN:-$ROOT/target/svnadmin/bin/svnadmin}"
SERVER_BIN="${SERVER_BIN:-$ROOT/target/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/svn/bin/svn}"

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

# tlib_stop_server — kill $SRV (set inline by callers). Idempotent.
tlib_stop_server() {
    if [ -n "${SRV:-}" ]; then
        kill "$SRV" 2>/dev/null || true
        wait "$SRV" 2>/dev/null || true
        SRV=""
    fi
}
