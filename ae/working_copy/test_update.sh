#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for svn update: two WCs, commit from one, update the
# other, verify changes appear. Also test the conflict rejection path.

source "$(dirname "$0")/../../tests/lib.sh"

tlib_use_fixture test_update
WC1=/tmp/svnae_test_upd_wc1
WC2=/tmp/svnae_test_upd_wc2

URL="http://127.0.0.1:$PORT/demo"

rm -rf "$WC1" "$WC2"

"$SVN_BIN" checkout "$URL" "$WC1" >/dev/null
"$SVN_BIN" checkout "$URL" "$WC2" >/dev/null

# Both WCs start at base_rev 3. Verify.
b1=$(sqlite3 "$WC1/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
b2=$(sqlite3 "$WC2/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
tlib_check "wc1 base_rev 3" "3" "$b1"
tlib_check "wc2 base_rev 3" "3" "$b2"

# --- wc1: modify README, add new file, delete src/main.c, commit ---
cd "$WC1"
echo "updated from wc1" > README
echo "new file body"    > NEWFILE
"$SVN_BIN" add NEWFILE   > /dev/null
"$SVN_BIN" rm src/main.c > /dev/null
out=$("$SVN_BIN" commit --author alice --log "wc1 changes")
new_rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "wc1 commit -> rev 4" "4" "$new_rev"

# --- wc2: plain update (no local changes) should pull all of wc1's work ---
cd "$WC2"
out=$("$SVN_BIN" update)
tlib_check "wc2 update announces rev" "At revision 4." "$out"

# Verify disk state in wc2.
tlib_check "wc2 README updated"    "updated from wc1" "$(cat README)"
tlib_check "wc2 NEWFILE present"   "new file body"    "$(cat NEWFILE)"
tlib_check "wc2 src/main.c gone"   "absent"           "$(test -f src/main.c && echo present || echo absent)"

# base_rev advanced.
b2new=$(sqlite3 "$WC2/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
tlib_check "wc2 base_rev 4"        "4"                "$b2new"

# wc.db should now have NEWFILE tracked.
tracked=$(sqlite3 "$WC2/.svn/wc.db" "SELECT path FROM nodes WHERE path='NEWFILE'")
tlib_check "wc2 NEWFILE in db"     "NEWFILE"          "$tracked"

# status should be clean.
out=$("$SVN_BIN" status)
tlib_check "wc2 clean after update" "" "$out"

# --- conflict scenario: edit README in wc2, commit a new README in wc1,
#     then `svn update` in wc2 should produce a 3-way merge with
#     conflict markers, mark the node C, and refuse the next commit. ---
cd "$WC1"
echo "wc1 second change" > README
out=$("$SVN_BIN" commit --author alice --log "conflict setter")
rev_c=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "wc1 conflict-setter rev" "5" "$rev_c"

cd "$WC2"
echo "wc2 divergent change" > README
# Update succeeds — applies a 3-way merge with markers for README.
out=$("$SVN_BIN" update 2>&1)
tlib_check "update succeeded"      "At revision 5." "$(echo "$out" | grep '^At revision')"

# README should have conflict markers from diff3 -m.
grep -q '<<<<<<< MINE' README && grep -q '>>>>>>> THEIRS' README && m=yes || m=no
tlib_check "README has conflict markers" "yes" "$m"

# base_rev advanced despite conflict.
b2_after=$(sqlite3 "$WC2/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
tlib_check "wc2 base_rev advanced to 5" "5" "$b2_after"

# Sidecars exist.
tlib_check ".mine sidecar"          "present" "$(test -f README.mine && echo present || echo absent)"
tlib_check ".r4 sidecar (BASE)"     "present" "$(test -f README.r4 && echo present || echo absent)"
tlib_check ".r5 sidecar (THEIRS)"   "present" "$(test -f README.r5 && echo present || echo absent)"

# status shows C.
out=$("$SVN_BIN" status)
tlib_check "status shows C README"  "1" "$(echo "$out" | grep -c '^C.*README' || true)"

# Sidecars must NOT show as ? in status.
tlib_check "no ? entries"           "0" "$(echo "$out" | grep -c '^?' || true)"

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
tlib_check "resolve prints Resolved" "Resolved conflicted state of 'README'" "$(cat /tmp/res.out)"
tlib_check "sidecar .mine gone"     "absent" "$(test -f README.mine && echo present || echo absent)"
tlib_check "sidecar .r4 gone"       "absent" "$(test -f README.r4 && echo present || echo absent)"
tlib_check "sidecar .r5 gone"       "absent" "$(test -f README.r5 && echo present || echo absent)"

# Status: README is now M (resolved content differs from pristine).
out=$("$SVN_BIN" status)
tlib_check "resolved shows M"       "1" "$(echo "$out" | grep -c '^M.*README' || true)"

# Commit now succeeds.
out=$("$SVN_BIN" commit --author alice --log "resolve to reconciled")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "commit after resolve"   "6" "$rev"

# --- --rev N selector still works ---
out=$("$SVN_BIN" update --rev 4)
tlib_check "update --rev 4"        "At revision 4."   "$out"
tlib_check "wc2 README is rev-4"   "updated from wc1" "$(cat README)"

cd /

tlib_summary "test_wc_update"