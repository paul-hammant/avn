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

# svn:ignore: svn status should skip '?' entries whose name matches any
# glob in the parent dir's svn:ignore property.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9460}"
REPO=/tmp/svnae_test_ig_repo
WC=/tmp/svnae_test_ig_wc
SERVER_BIN=/tmp/svnae_test_ig_server
SEED_BIN=/tmp/svnae_test_ig_seed
SVN_BIN=/tmp/svnae_test_ig_svn

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
./regen.sh >/dev/null
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_ig_server.log 2>&1 &
SRV=$!
sleep 1.5
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# src/ exists in the seed. Create unversioned files under it.
echo "a" > src/something.o
echo "b" > src/other.o
echo "c" > src/notes.txt

# Before setting svn:ignore: all three show as '?' entries.
out=$("$SVN_BIN" status)
n_q=$(echo "$out" | grep -c '^?' || true)
check "3 unversioned before ignore"   "3" "$n_q"

# Set svn:ignore on src/ to skip *.o
"$SVN_BIN" propset svn:ignore '*.o' src >/dev/null

# Now status should only show notes.txt.
out=$("$SVN_BIN" status)
n_q=$(echo "$out" | grep -c '^?' || true)
check "1 unversioned after *.o"        "1" "$n_q"
notes=$(echo "$out" | grep notes.txt | awk '{print $2}')
check "notes.txt still listed"         "src/notes.txt" "$notes"

# Multi-pattern svn:ignore
"$SVN_BIN" propset svn:ignore $'*.o\nnotes.txt' src >/dev/null
out=$("$SVN_BIN" status)
n_q=$(echo "$out" | grep -c '^?' || true)
check "0 unversioned with both patterns" "0" "$n_q"

# Empty patterns / blank lines are tolerated.
"$SVN_BIN" propset svn:ignore $'\n*.o\n\n' src >/dev/null
out=$("$SVN_BIN" status)
has_notes=$(echo "$out" | grep -c '^?.*notes.txt' || true)
check "blank lines tolerated"          "1" "$has_notes"

# svn:ignore only applies to immediate children — nested dirs are a
# separate ignore scope. Verify that files directly under src/ with
# a *.o name are hidden even when svn:ignore is set at root too.
echo "x" > top-ignore-me.o
# No svn:ignore at root — top-ignore-me.o still visible.
out=$("$SVN_BIN" status)
check "root *.o still visible (no ignore at root)" "1" "$(echo "$out" | grep -c 'top-ignore-me.o' || true)"

rm top-ignore-me.o src/something.o src/other.o src/notes.txt
"$SVN_BIN" propdel svn:ignore src >/dev/null
out=$("$SVN_BIN" status)
check "clean after cleanup"            "" "$out"

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
echo "test_wc_ignore: OK"
