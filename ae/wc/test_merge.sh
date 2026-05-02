#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# svn merge URL@REVA:REVB [TARGET_DIR]
#
# Scenario:
#   seed           — r0..r3 with src/main.c content return 42
#   cp src -> src-branch     — r4 (server-side copy; branch)
#   trunk edit src/main.c    — r5 (trunk diverges)
#   in a WC, merge src@4:5 into src-branch
#   -> src-branch/main.c now has trunk's r5 content
#   -> svn:mergeinfo recorded
#   commit merge             — r6

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9700}"
REPO=/tmp/svnae_test_mrg_repo
WC=/tmp/svnae_test_mrg_wc

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_mrg_server.log 2>&1 &
SRV=$!
sleep 1.5

# r4: branch src -> src-branch
"$SVN_BIN" cp "$URL/src" "$URL/src-branch" --author alice --log "branch src" >/dev/null

# r5: change src/main.c on trunk (via a temp WC).
tmp_wc=/tmp/svnae_test_mrg_trunk
rm -rf "$tmp_wc"
"$SVN_BIN" checkout "$URL" "$tmp_wc" >/dev/null
(cd "$tmp_wc" && echo "trunk improvement" > src/main.c && "$SVN_BIN" commit --author alice --log "trunk edit r5" >/dev/null)
rm -rf "$tmp_wc"

# Check out a WC at r5 to do the merge.
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

# Pre-merge: src-branch/main.c is still the old content (r4 = pre-trunk-edit).
tlib_check "branch main.c pre-merge" "int main() { return 42; }" "$(cat src-branch/main.c)"

# Merge src@4:5 into src-branch — the canonical branch-merge workflow.
"$SVN_BIN" merge "$URL/src@4:5" src-branch > /tmp/mrg.out 2>&1 || { cat /tmp/mrg.out; false; }

# Branch's main.c now has the trunk's r5 content.
tlib_check "branch main.c post-merge" "trunk improvement" "$(cat src-branch/main.c)"
# Trunk src/main.c in this WC also has it (it was already at r5).
tlib_check "trunk main.c still r5"   "trunk improvement" "$(cat src/main.c)"

# Status shows M on src-branch/main.c. src/main.c is NOT modified by
# the merge since we merged INTO src-branch, not into ".".
out=$("$SVN_BIN" status)
tlib_check "M on branch file"        "1" "$(echo "$out" | grep -c '^M.*src-branch/main.c' || true)"
tlib_check "src/main.c not M"        "0" "$(echo "$out" | grep -c '^M.*src/main.c' || true)"

# svn:mergeinfo recorded on the WC root (path "").
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "mergeinfo recorded"      "src:5-5" "$mi"

# Commit the merge.
out=$("$SVN_BIN" commit --author alice --log "merge trunk r5 into branch")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "merge commit rev"        "6" "$rev"

# Conflict scenario: local edit on the target file, then merge — now
# produces a 3-way merge with conflict markers + C status (Phase 5.13).
cd /
rm -rf "$WC"
"$SVN_BIN" checkout "$URL" "$WC" --rev 4 >/dev/null
cd "$WC"
echo "local divergent" > src-branch/main.c
# Merge runs (no fatal abort). It should report C and leave markers.
"$SVN_BIN" merge "$URL/src@4:5" src-branch 2>/tmp/mrg_err || true

# File has conflict markers.
grep -q '<<<<<<< MINE' src-branch/main.c && grep -q '>>>>>>> THEIRS' src-branch/main.c && m=yes || m=no
tlib_check "merge produced markers" "yes" "$m"

# status shows C.
out=$("$SVN_BIN" status)
tlib_check "status shows C"         "1" "$(echo "$out" | grep -c '^C.*src-branch/main.c' || true)"

# Commit refuses.
if "$SVN_BIN" commit --author alice --log "try" 2>/tmp/commit_err; then
    echo "  FAIL commit-with-conflict should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   commit-with-conflict refused"
fi

# Resolve with --accept mine (keep local edit) and commit.
"$SVN_BIN" resolve src-branch/main.c --accept mine > /dev/null
tlib_check "resolved to mine"       "local divergent" "$(cat src-branch/main.c)"
out=$("$SVN_BIN" status)
# After resolve-mine the file sha differs from pristine (pristine = THEIRS).
tlib_check "resolved shows M"       "1" "$(echo "$out" | grep -c '^M.*src-branch/main.c' || true)"

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

tlib_summary "test_wc_merge"