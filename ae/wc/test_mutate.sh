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

# Tests for svn add / rm against a checked-out WC.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9390}"
REPO=/tmp/svnae_test_mut_repo
WC=/tmp/svnae_test_mut_wc
SERVER_BIN=/tmp/svnae_test_mut_server
SEED_BIN=/tmp/svnae_test_mut_seed
SVN_BIN=/tmp/svnae_test_mut_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null

"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_mut_server.log 2>&1 &
SRV=$!
sleep 1.5

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

cd "$WC"

# --- add a new file ---
echo "hello from cli" > NOTES
out=$("$SVN_BIN" status | sort)
# NOTES shows as '?' before add.
check "? NOTES pre-add"    "?       NOTES" "$(echo "$out" | grep NOTES)"

"$SVN_BIN" add NOTES > /tmp/mut_add.out
check "add prints A"       "A         NOTES" "$(cat /tmp/mut_add.out)"

out=$("$SVN_BIN" status | sort)
check "A NOTES post-add"   "A       NOTES" "$(echo "$out" | grep NOTES)"

# --- rm a previously-committed file ---
"$SVN_BIN" rm README > /tmp/mut_rm.out
check "rm prints D"        "D         README" "$(cat /tmp/mut_rm.out)"

out=$("$SVN_BIN" status)
check "README gone on disk" "absent" "$(test -f README && echo present || echo absent)"
check "D README in status" "1" "$(echo "$out" | grep -c '^D.*README' || true)"

# --- add something that's not on disk ---
if "$SVN_BIN" add does-not-exist 2>/dev/null; then
    echo "  FAIL add of nonexistent should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   add of nonexistent fails"
fi

# --- rm an already-added file (should just drop the row, keep file) ---
echo "draft" > DRAFT
"$SVN_BIN" add DRAFT >/dev/null
"$SVN_BIN" rm DRAFT >/dev/null
check "DRAFT kept on disk" "present" "$(test -f DRAFT && echo present || echo absent)"
out=$("$SVN_BIN" status)
# After rm of an added (not-yet-committed) file: row dropped → DRAFT is
# unversioned ('?') again.
check "DRAFT back to ?" "1" "$(echo "$out" | grep -c '^?.*DRAFT' || true)"

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
echo "test_wc_mutate: OK"
