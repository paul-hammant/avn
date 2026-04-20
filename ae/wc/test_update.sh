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
#     then `svn update` in wc2 should produce a 3-way merge with
#     conflict markers, mark the node C, and refuse the next commit. ---
cd "$WC1"
echo "wc1 second change" > README
out=$("$SVN_BIN" commit --author alice --log "conflict setter")
rev_c=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "wc1 conflict-setter rev" "5" "$rev_c"

cd "$WC2"
echo "wc2 divergent change" > README
# Update succeeds — applies a 3-way merge with markers for README.
out=$("$SVN_BIN" update 2>&1)
check "update succeeded"      "At revision 5." "$(echo "$out" | grep '^At revision')"

# README should have conflict markers from diff3 -m.
grep -q '<<<<<<< MINE' README && grep -q '>>>>>>> THEIRS' README && m=yes || m=no
check "README has conflict markers" "yes" "$m"

# base_rev advanced despite conflict.
b2_after=$(sqlite3 "$WC2/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
check "wc2 base_rev advanced to 5" "5" "$b2_after"

# Sidecars exist.
check ".mine sidecar"          "present" "$(test -f README.mine && echo present || echo absent)"
check ".r4 sidecar (BASE)"     "present" "$(test -f README.r4 && echo present || echo absent)"
check ".r5 sidecar (THEIRS)"   "present" "$(test -f README.r5 && echo present || echo absent)"

# status shows C.
out=$("$SVN_BIN" status)
check "status shows C README"  "1" "$(echo "$out" | grep -c '^C.*README' || true)"

# Sidecars must NOT show as ? in status.
check "no ? entries"           "0" "$(echo "$out" | grep -c '^?' || true)"

# commit is refused while the conflict stands.
if "$SVN_BIN" commit --author alice --log "try anyway" 2>/tmp/commit_err; then
    echo "  FAIL commit with conflict should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   commit with conflict refused"
fi

# --- svn resolve --accept working: accept user's hand-edit ---
echo "reconciled" > README
"$SVN_BIN" resolve --accept working README > /tmp/res.out
check "resolve prints Resolved" "Resolved conflicted state of 'README'" "$(cat /tmp/res.out)"
check "sidecar .mine gone"     "absent" "$(test -f README.mine && echo present || echo absent)"
check "sidecar .r4 gone"       "absent" "$(test -f README.r4 && echo present || echo absent)"
check "sidecar .r5 gone"       "absent" "$(test -f README.r5 && echo present || echo absent)"

# Status: README is now M (resolved content differs from pristine).
out=$("$SVN_BIN" status)
check "resolved shows M"       "1" "$(echo "$out" | grep -c '^M.*README' || true)"

# Commit now succeeds.
out=$("$SVN_BIN" commit --author alice --log "resolve to reconciled")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "commit after resolve"   "6" "$rev"

# --- --rev N selector still works ---
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
