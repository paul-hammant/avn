#!/bin/bash
# End-to-end checkout test: server is seeded, svn CLI checks out, we
# verify the files on disk, wc.db rows, and pristine blobs.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9370}"
REPO=/tmp/svnae_test_co_repo
WC=/tmp/svnae_test_co_wc
SERVER_BIN=/tmp/svnae_test_co_server
SEED_BIN=/tmp/svnae_test_co_seed
SVN_BIN=/tmp/svnae_test_co_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null

"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_co_server.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- checkout head ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
check "README exists"      "Hello"                    "$(cat "$WC/README")"
check "src/main.c exists"  "int main() { return 42; }" "$(cat "$WC/src/main.c")"
check "LICENSE absent"     "absent"                   "$(test -f "$WC/LICENSE" && echo present || echo absent)"
check "src is dir"         "dir"                      "$(test -d "$WC/src" && echo dir || echo nondir)"

# --- .svn/wc.db populated ---
n_nodes=$(sqlite3 "$WC/.svn/wc.db" "SELECT COUNT(*) FROM nodes")
check "nodes count"        "3"                        "$n_nodes"

url_info=$(sqlite3 "$WC/.svn/wc.db" "SELECT value FROM info WHERE key='url'")
check "info url"           "$URL"                     "$url_info"

rev_info=$(sqlite3 "$WC/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
check "info base_rev"      "3"                        "$rev_info"

# --- pristine store populated ---
n_pristine=$(find "$WC/.svn/pristine" -name '*.rep' | wc -l)
check "pristine count"     "2"                        "$n_pristine"

# --- checkout --rev 1 into a second WC ---
WC1="${WC}_r1"
rm -rf "$WC1"
"$SVN_BIN" checkout "$URL" "$WC1" --rev 1 >/dev/null
check "r1 LICENSE present" "Apache-2.0"               "$(cat "$WC1/LICENSE")"
check "r1 main.c old"      "int main() { return 0; }" "$(cat "$WC1/src/main.c")"

rev_info1=$(sqlite3 "$WC1/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
check "r1 info base_rev"   "1"                        "$rev_info1"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC" "$WC1"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_wc_checkout: OK"
