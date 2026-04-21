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

# Server-side copy = branching. Verifies:
#   - svn cp URL URL creates a new revision whose tree contains the
#     copied subtree at the destination path.
#   - The original tree is untouched.
#   - No new .rep files are created for file content (full subtree
#     rep-sharing via same-sha1 entries).
#   - Both branches can subsequently evolve independently.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9470}"
REPO=/tmp/svnae_test_br_repo
WC=/tmp/svnae_test_br_wc
SERVER_BIN=/tmp/svnae_test_br_server
SEED_BIN=/tmp/svnae_test_br_seed
SVN_BIN=/tmp/svnae_test_br_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_br_server.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# Seeder creates head=3. Branch src -> src-branch.
before=$(find "$REPO/reps" -name '*.rep' | wc -l)

out=$("$SVN_BIN" cp "$URL/src" "$URL/src-branch" --author alice --log "branch off src")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "branch commit rev"        "4"                        "$rev"

after=$(find "$REPO/reps" -name '*.rep' | wc -l)
delta=$((after - before))
# Expected: new root-dir blob + new revision blob = 2 new reps.
# The src-branch subtree itself reuses the existing `src` dir blob,
# so the file blobs (main.c, lib/...) are all shared via sha1.
check "only 2 new reps"          "2"                        "$delta"

# List shows the new branch entry.
out=$("$SVN_BIN" ls "$URL")
check "ls shows src-branch/"     "1"                        "$(echo "$out" | grep -c 'src-branch/' || true)"

# Contents match between trunk and branch at r4.
a=$("$SVN_BIN" cat "$URL" src/main.c)
b=$("$SVN_BIN" cat "$URL" src-branch/main.c)
check "branch main.c matches src"   "$a"                   "$b"

# r3 still unchanged — no src-branch there.
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/3/list/src-branch")
check "r3 has no src-branch"     "404"                      "$code"

# Evolve trunk and branch independently via a checked-out WC.
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
# Modify trunk's main.c
echo "trunk change" > src/main.c
out=$("$SVN_BIN" commit --author alice --log "trunk edit")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "trunk commit"             "5"                        "$rev"

# Branch's main.c should still have the pre-branch content.
b=$("$SVN_BIN" cat "$URL" src-branch/main.c)
check "branch unaffected by trunk edit" \
                                 "int main() { return 42; }" \
                                 "$b"

# Edit only on the branch.
# We need a WC of the branch portion. For Phase 5.12 our WC checkout is
# always the repo root, so edit via the WC's src-branch/ subpath.
cd "$WC"
echo "branch change" > src-branch/main.c
out=$("$SVN_BIN" commit --author alice --log "branch edit")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "branch commit"            "6"                        "$rev"

# Now trunk has "trunk change" (r5) and branch has "branch change" (r6).
a=$("$SVN_BIN" cat "$URL" src/main.c)
b=$("$SVN_BIN" cat "$URL" src-branch/main.c)
check "trunk final"              "trunk change"             "$a"
check "branch final"             "branch change"            "$b"
check "trunk and branch differ"  "differ"                   "$( [ "$a" = "$b" ] && echo same || echo differ)"

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
echo "test_wc_branch: OK"
