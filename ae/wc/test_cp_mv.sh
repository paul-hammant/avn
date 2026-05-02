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

# End-to-end: checkout a WC, cp + mv tracked files, verify db rows +
# disk + that a commit round-trips the new tree.
set -e
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

PORT="${PORT:-9440}"
REPO=/tmp/svnae_test_cpmv_repo
WC=/tmp/svnae_test_cpmv_wc
SERVER_BIN="${SERVER_BIN:-$ROOT/target/ae/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/ae/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/ae/svn/bin/svn}"

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT


rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_cpmv_server.log 2>&1 &
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

# --- cp README to README.bak ---
out=$("$SVN_BIN" cp README README.bak)
check "cp prints A dst"        "A         README.bak" "$out"
check "README still there"     "Hello" "$(cat README)"
check "README.bak content"     "Hello" "$(cat README.bak)"

# Status: README.bak is added, README unchanged.
out=$("$SVN_BIN" status | sort)
check "status shows A README.bak" "A       README.bak" "$(echo "$out" | grep README.bak)"

# --- mv: rename src/main.c to src/new_main.c ---
out=$("$SVN_BIN" mv src/main.c src/new_main.c)
check "mv prints A + D"        "A         src/new_main.c
D         src/main.c"          "$out"
check "src/main.c gone"        "absent"               "$(test -f src/main.c && echo present || echo absent)"
check "src/new_main.c present" "int main() { return 42; }" "$(cat src/new_main.c)"

# Status shows both.
out=$("$SVN_BIN" status)
check "A new_main.c in status" "1" "$(echo "$out" | grep -c '^A.*src/new_main.c' || true)"
check "D main.c in status"     "1" "$(echo "$out" | grep -c '^D.*src/main.c'     || true)"

# --- cp of unknown source fails ---
if "$SVN_BIN" cp nosuchfile someplace 2>/dev/null; then
    echo "  FAIL cp of unknown should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   cp of unknown fails"
fi

# --- cp onto existing dst fails ---
if "$SVN_BIN" cp README src/new_main.c 2>/dev/null; then
    echo "  FAIL cp onto existing should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   cp onto existing fails"
fi

# --- commit and verify remote ---
out=$("$SVN_BIN" commit --author cpuser --log "cp + mv")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "commit returns rev 4"   "4" "$rev"

check "remote README.bak"     "Hello" "$("$SVN_BIN" cat "$URL" README.bak)"
check "remote new_main.c"     "int main() { return 42; }" "$("$SVN_BIN" cat "$URL" src/new_main.c)"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/src/main.c")
check "remote main.c gone"    "404" "$code"

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
echo "test_wc_cp_mv: OK"
