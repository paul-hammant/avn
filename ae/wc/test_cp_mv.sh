#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end: checkout a WC, cp + mv tracked files, verify db rows +
# disk + that a commit round-trips the new tree.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="$test_cp_mv_PORT"
REPO="$test_cp_mv_REPO"
WC=/tmp/svnae_test_cpmv_wc

URL="http://127.0.0.1:$PORT/demo"
rm -rf "$WC"

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

# --- cp README to README.bak ---
out=$("$SVN_BIN" cp README README.bak)
tlib_check "cp prints A dst"        "A         README.bak" "$out"
tlib_check "README still there"     "Hello" "$(cat README)"
tlib_check "README.bak content"     "Hello" "$(cat README.bak)"

# Status: README.bak is added, README unchanged.
out=$("$SVN_BIN" status | sort)
tlib_check "status shows A README.bak" "A       README.bak" "$(echo "$out" | grep README.bak)"

# --- mv: rename src/main.c to src/new_main.c ---
out=$("$SVN_BIN" mv src/main.c src/new_main.c)
tlib_check "mv prints A + D"        "A         src/new_main.c
D         src/main.c"          "$out"
tlib_check "src/main.c gone"        "absent"               "$(test -f src/main.c && echo present || echo absent)"
tlib_check "src/new_main.c present" "int main() { return 42; }" "$(cat src/new_main.c)"

# Status shows both.
out=$("$SVN_BIN" status)
tlib_check "A new_main.c in status" "1" "$(echo "$out" | grep -c '^A.*src/new_main.c' || true)"
tlib_check "D main.c in status"     "1" "$(echo "$out" | grep -c '^D.*src/main.c'     || true)"

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
tlib_check "commit returns rev 4"   "4" "$rev"

tlib_check "remote README.bak"     "Hello" "$("$SVN_BIN" cat "$URL" README.bak)"
tlib_check "remote new_main.c"     "int main() { return 42; }" "$("$SVN_BIN" cat "$URL" src/new_main.c)"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/src/main.c")
tlib_check "remote main.c gone"    "404" "$code"

cd /

tlib_summary "test_wc_cp_mv"