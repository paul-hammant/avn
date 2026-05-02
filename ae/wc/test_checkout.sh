#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end checkout test: server is seeded, svn CLI checks out, we
# verify the files on disk, wc.db rows, and pristine blobs.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="$test_checkout_PORT"
REPO="$test_checkout_REPO"
WC=/tmp/svnae_test_co_wc

URL="http://127.0.0.1:$PORT/demo"

rm -rf "$WC"
tlib_seed "$REPO"
tlib_start_server "$PORT" "$REPO"

# --- checkout head ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
tlib_check "README exists"      "Hello"                    "$(cat "$WC/README")"
tlib_check "src/main.c exists"  "int main() { return 42; }" "$(cat "$WC/src/main.c")"
tlib_check "LICENSE absent"     "absent"                   "$(test -f "$WC/LICENSE" && echo present || echo absent)"
tlib_check "src is dir"         "dir"                      "$(test -d "$WC/src" && echo dir || echo nondir)"

# --- .svn/wc.db populated ---
n_nodes=$(sqlite3 "$WC/.svn/wc.db" "SELECT COUNT(*) FROM nodes")
tlib_check "nodes count"        "3"                        "$n_nodes"

url_info=$(sqlite3 "$WC/.svn/wc.db" "SELECT value FROM info WHERE key='url'")
tlib_check "info url"           "$URL"                     "$url_info"

rev_info=$(sqlite3 "$WC/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
tlib_check "info base_rev"      "3"                        "$rev_info"

# --- pristine store populated ---
n_pristine=$(find "$WC/.svn/pristine" -name '*.rep' | wc -l)
tlib_check "pristine count"     "2"                        "$n_pristine"

# --- checkout --rev 1 into a second WC ---
WC1="${WC}_r1"
rm -rf "$WC1"
"$SVN_BIN" checkout "$URL" "$WC1" --rev 1 >/dev/null
tlib_check "r1 LICENSE present" "Apache-2.0"               "$(cat "$WC1/LICENSE")"
tlib_check "r1 main.c old"      "int main() { return 0; }" "$(cat "$WC1/src/main.c")"

rev_info1=$(sqlite3 "$WC1/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
tlib_check "r1 info base_rev"   "1"                        "$rev_info1"

tlib_summary "test_wc_checkout"