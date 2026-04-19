#!/bin/bash
# End-to-end test for svn update: two WCs, commit from one, update the
# other, verify changes appear. Also test the conflict rejection path.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9410}"
REPO=/tmp/svnae_test_upd_repo
WC1=/tmp/svnae_test_upd_wc1
WC2=/tmp/svnae_test_upd_wc2
SERVER_BIN=/tmp/svnae_test_upd_server
SEED_BIN=/tmp/svnae_test_upd_seed
SVN_BIN=/tmp/svnae_test_upd_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC1" "$WC2"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_upd_server.log 2>&1 &
SRV=$!
sleep 1.5

"$SVN_BIN" checkout "$URL" "$WC1" >/dev/null
"$SVN_BIN" checkout "$URL" "$WC2" >/dev/null

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# Both WCs start at base_rev 3. Verify.
b1=$(sqlite3 "$WC1/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
b2=$(sqlite3 "$WC2/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
check "wc1 base_rev 3" "3" "$b1"
check "wc2 base_rev 3" "3" "$b2"

# --- wc1: modify README, add new file, delete src/main.c, commit ---
cd "$WC1"
echo "updated from wc1" > README
echo "new file body"    > NEWFILE
"$SVN_BIN" add NEWFILE   > /dev/null
"$SVN_BIN" rm src/main.c > /dev/null
out=$("$SVN_BIN" commit --author alice --log "wc1 changes")
new_rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "wc1 commit -> rev 4" "4" "$new_rev"

# --- wc2: plain update (no local changes) should pull all of wc1's work ---
cd "$WC2"
out=$("$SVN_BIN" update)
check "wc2 update announces rev" "At revision 4." "$out"

# Verify disk state in wc2.
check "wc2 README updated"    "updated from wc1" "$(cat README)"
check "wc2 NEWFILE present"   "new file body"    "$(cat NEWFILE)"
check "wc2 src/main.c gone"   "absent"           "$(test -f src/main.c && echo present || echo absent)"

# base_rev advanced.
b2new=$(sqlite3 "$WC2/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
check "wc2 base_rev 4"        "4"                "$b2new"

# wc.db should now have NEWFILE tracked.
tracked=$(sqlite3 "$WC2/.svn/wc.db" "SELECT path FROM nodes WHERE path='NEWFILE'")
check "wc2 NEWFILE in db"     "NEWFILE"          "$tracked"

# status should be clean.
out=$("$SVN_BIN" status)
check "wc2 clean after update" "" "$out"

# --- conflict scenario: edit README in wc2, commit a new README in wc1,
#     then `svn update` in wc2 should report conflict and leave wc2 as-is. ---
cd "$WC1"
echo "wc1 second change" > README
out=$("$SVN_BIN" commit --author alice --log "conflict setter")
rev_c=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "wc1 conflict-setter rev" "5" "$rev_c"

cd "$WC2"
echo "wc2 divergent change" > README
# Expect update to fail with conflict.
if "$SVN_BIN" update 2>/tmp/upd_err; then
    echo "  FAIL update should have returned nonzero"
    FAILS=$((FAILS+1))
else
    echo "  ok   update returns nonzero on conflict"
fi

# Nothing should have changed in wc2: README is still our divergent version.
check "wc2 README unchanged on conflict" "wc2 divergent change" "$(cat README)"
b2_after=$(sqlite3 "$WC2/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
check "wc2 base_rev unchanged on conflict" "4" "$b2_after"

# --- resolve by reverting our edit, then update succeeds ---
echo "updated from wc1" > README   # restore to rev 4 content
out=$("$SVN_BIN" update)
check "update succeeds after resolve" "At revision 5." "$out"
check "wc2 README is the rev-5 version" "wc1 second change" "$(cat README)"

# --- --rev N selector: go back down to rev 4 ---
out=$("$SVN_BIN" update --rev 4)
check "update --rev 4"        "At revision 4."   "$out"
check "wc2 README is rev-4"   "updated from wc1" "$(cat README)"

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC1" "$WC2"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_wc_update: OK"
