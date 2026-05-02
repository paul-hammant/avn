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

# tlib_seed REPO — wipe and seed a repo with the canonical 3-commit
# tree from svnae-seed. Most tests start with this.
tlib_seed() {
    local repo="$1"
    rm -rf "$repo"
    "$SEED_BIN" "$repo" >/dev/null
}

# tlib_start_server PORT REPO [REPO_NAME] [extra args...]
# REPO_NAME defaults to "demo". Spawns aether-svnserver in background,
# logs to /tmp/svnae_srv_PORT.log, sleeps 1.5s for startup. The pid
# lands in $TLIB_SRV; for backwards-compat callers also set $SRV.
# Sets a trap to pkill on EXIT.
tlib_start_server() {
    local port="$1" repo="$2" name="${3:-demo}"
    shift 3 2>/dev/null || shift "$#"   # tolerate caller passing only 2 args
    "$SERVER_BIN" "$name" "$repo" "$port" "$@" >"/tmp/svnae_srv_${port}.log" 2>&1 &
    TLIB_SRV=$!
    SRV=$TLIB_SRV
    # shellcheck disable=SC2064
    trap "pkill -f \"\${SERVER_BIN} .* ${port}\" 2>/dev/null || true" EXIT
    sleep 1.5
}

# tlib_stop_server — kills $TLIB_SRV (set by tlib_start_server).
# Tests that spin up multiple servers manage the others themselves.
tlib_stop_server() {
    if [ -n "$TLIB_SRV" ]; then
        kill "$TLIB_SRV" 2>/dev/null || true
        wait "$TLIB_SRV" 2>/dev/null || true
        TLIB_SRV=""
        SRV=""
    fi
}
