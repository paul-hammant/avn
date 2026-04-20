#!/bin/bash
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
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9480}"
REPO=/tmp/svnae_test_mrg_repo
WC=/tmp/svnae_test_mrg_wc
SERVER_BIN=/tmp/svnae_test_mrg_server
SEED_BIN=/tmp/svnae_test_mrg_seed
SVN_BIN=/tmp/svnae_test_mrg_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_mrg_server.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

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
check "branch main.c pre-merge" "int main() { return 42; }" "$(cat src-branch/main.c)"

# Merge src@4:5 into src-branch — the canonical branch-merge workflow.
"$SVN_BIN" merge "$URL/src@4:5" src-branch > /tmp/mrg.out 2>&1 || { cat /tmp/mrg.out; false; }

# Branch's main.c now has the trunk's r5 content.
check "branch main.c post-merge" "trunk improvement" "$(cat src-branch/main.c)"
# Trunk src/main.c in this WC also has it (it was already at r5).
check "trunk main.c still r5"   "trunk improvement" "$(cat src/main.c)"

# Status shows M on src-branch/main.c. src/main.c is NOT modified by
# the merge since we merged INTO src-branch, not into ".".
out=$("$SVN_BIN" status)
check "M on branch file"        "1" "$(echo "$out" | grep -c '^M.*src-branch/main.c' || true)"
check "src/main.c not M"        "0" "$(echo "$out" | grep -c '^M.*src/main.c' || true)"

# svn:mergeinfo recorded on the WC root (path "").
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
check "mergeinfo recorded"      "src:5-5" "$mi"

# Commit the merge.
out=$("$SVN_BIN" commit --author alice --log "merge trunk r5 into branch")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "merge commit rev"        "6" "$rev"

# Conflict: local edit on the target file, then merge — should refuse.
cd /
rm -rf "$WC"
"$SVN_BIN" checkout "$URL" "$WC" --rev 4 >/dev/null
cd "$WC"
echo "local divergent" > src-branch/main.c
if "$SVN_BIN" merge "$URL/src@4:5" src-branch 2>/tmp/mrg_err; then
    echo "  FAIL merge with local mod should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   merge with local mod rejected"
fi
check "local edit preserved"    "local divergent" "$(cat src-branch/main.c)"

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_wc_merge: OK"
