# Shared bash test helpers. Sourced from two contexts:
#
#  1. A test_*.sh script under ae/<dir>/    (BASH_SOURCE[1] points there)
#  2. aeb's pre_command via .aeb/lib/svnae/  (cwd is already repo root)
#
# After sourcing, $ROOT is the repo root and these are pre-set with
# canonical default paths to the four port binaries (env-overridable):
#
#   $ADMIN_BIN   target/ae/svnadmin/bin/svnadmin
#   $SERVER_BIN  target/ae/svnserver/bin/aether-svnserver
#   $SEED_BIN    target/ae/svnserver/bin/svnae-seed
#   $SVN_BIN     target/ae/svn/bin/svn
#
# Test bodies (inside test_*.sh):
#
#   tlib_check LABEL EXPECTED ACTUAL
#   tlib_summary "test_name"
#
# Inline-spawn helpers (kept for tests not yet on the SDK):
#
#   tlib_seed REPO                       # rm -rf REPO; seed it
#   tlib_start_server PORT REPO [args]   # spawn server, $SRV gets the pid,
#                                        # trap pkill on EXIT, sleep 1.5
#   tlib_stop_server                     # kill $SRV, wait for it
#
# SDK-driven fixture (used from .aeb/lib/svnae's pre/post hooks):
#
#   tlib_seed_named NAME REPO              # like tlib_seed but exports ${NAME}_REPO
#   tlib_fixture_server NAME PORT [args]   # spawn server (reads ${NAME}_REPO);
#                                          # export ${NAME}_PORT/_PID/_LOG
#   tlib_kill_servers                      # kill every fixture server

if [ -n "${BASH_SOURCE[1]:-}" ] && [ -f "${BASH_SOURCE[1]}" ]; then
    cd "$(dirname "${BASH_SOURCE[1]}")/../.."
fi
ROOT="$(pwd)"

ADMIN_BIN="${ADMIN_BIN:-$ROOT/target/ae/svnadmin/bin/svnadmin}"
SERVER_BIN="${SERVER_BIN:-$ROOT/target/ae/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/ae/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/ae/svn/bin/svn}"

TLIB_PIDS=""

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

# --- Inline-spawn helpers (legacy; called from test bodies) ---

# tlib_seed REPO — wipe and seed the canonical 3-commit tree.
tlib_seed() {
    local repo="$1"
    rm -rf "$repo"
    "$SEED_BIN" "$repo" >/dev/null
}

# tlib_start_server PORT REPO [REPO_NAME] [extra args...]
# REPO_NAME defaults to "demo". $SRV gets the pid; trap pkill on EXIT.
tlib_start_server() {
    local port="$1" repo="$2" name="${3:-demo}"
    shift 3 2>/dev/null || shift "$#"
    "$SERVER_BIN" "$name" "$repo" "$port" "$@" >"/tmp/svnae_srv_${port}.log" 2>&1 &
    SRV=$!
    # shellcheck disable=SC2064
    trap "pkill -f \"\${SERVER_BIN} .* ${port}\" 2>/dev/null || true" EXIT
    sleep 1.5
}

# tlib_stop_server — kill $SRV (set by tlib_start_server). Idempotent.
tlib_stop_server() {
    if [ -n "${SRV:-}" ]; then
        kill "$SRV" 2>/dev/null || true
        wait "$SRV" 2>/dev/null || true
        SRV=""
    fi
}

# --- SDK-driven fixture helpers (called from .aeb/lib/svnae pre/post) ---

# tlib_seed_named NAME REPO — wipe REPO, seed it, export ${NAME}_REPO.
tlib_seed_named() {
    local name="$1" repo="$2"
    rm -rf "$repo"
    "$SEED_BIN" "$repo" >/dev/null
    export "${name}_REPO=$repo"
}

# tlib_fixture_server NAME PORT [extra-server-args...]
# Reads ${NAME}_REPO. Exports ${NAME}_PORT, ${NAME}_PID, ${NAME}_LOG.
# Server is invoked as `aether-svnserver demo $repo $port $@` — URL
# prefix "demo" is the convention every test already hardcodes; NAME
# is just the fixture handle.
tlib_fixture_server() {
    local name="$1" port="$2"
    shift 2
    local repo_var="${name}_REPO" log="/tmp/svnae_srv_${name}_${port}.log"
    local repo="${!repo_var}"
    if [ -z "$repo" ]; then
        echo "tlib_fixture_server: ${name}_REPO unset — call tlib_seed_named first" >&2
        return 2
    fi
    "$SERVER_BIN" demo "$repo" "$port" "$@" >"$log" 2>&1 &
    local pid=$!
    export "${name}_PORT=$port"
    export "${name}_PID=$pid"
    export "${name}_LOG=$log"
    TLIB_PIDS="$TLIB_PIDS $pid"
    sleep 1.5
}

# tlib_kill_servers — kill every fixture server. Idempotent.
tlib_kill_servers() {
    if [ -n "$TLIB_PIDS" ]; then
        # shellcheck disable=SC2086
        kill $TLIB_PIDS 2>/dev/null || true
        # shellcheck disable=SC2086
        wait $TLIB_PIDS 2>/dev/null || true
        TLIB_PIDS=""
    fi
}
