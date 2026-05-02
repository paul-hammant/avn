#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Portions copyright Apache Subversion project contributors (2001-2026).
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing
# permissions and limitations under the License.

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
ROOT="$(pwd)"

PORT="${PORT:-9700}"
REPO=/tmp/svnae_test_mrg_repo
WC=/tmp/svnae_test_mrg_wc
SERVER_BIN="${SERVER_BIN:-$ROOT/target/ae/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/ae/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/ae/svn/bin/svn}"

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT


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
check "merge produced markers" "yes" "$m"

# status shows C.
out=$("$SVN_BIN" status)
check "status shows C"         "1" "$(echo "$out" | grep -c '^C.*src-branch/main.c' || true)"

# Commit refuses.
if "$SVN_BIN" commit --author alice --log "try" 2>/tmp/commit_err; then
    echo "  FAIL commit-with-conflict should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   commit-with-conflict refused"
fi

# Resolve with --accept mine (keep local edit) and commit.
"$SVN_BIN" resolve src-branch/main.c --accept mine > /dev/null
check "resolved to mine"       "local divergent" "$(cat src-branch/main.c)"
out=$("$SVN_BIN" status)
# After resolve-mine the file sha differs from pristine (pristine = THEIRS).
check "resolved shows M"       "1" "$(echo "$out" | grep -c '^M.*src-branch/main.c' || true)"

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
