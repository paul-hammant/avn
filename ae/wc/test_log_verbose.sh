#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# svn log --verbose — per-revision A/M/D path list.
#
# Seeder creates:
#   r0: initial empty commit
#   r1: adds README, src/main.c, src/lib/helper.c   (A ...)
#   r2: modifies src/main.c                         (M src/main.c)
#   r3: further edit
# We then add r4..r6 with known actions to cover A/M/D explicitly.

source "$(dirname "$0")/../../tests/lib.sh"

tlib_use_fixture test_log_verbose
WC=/tmp/svnae_test_logv_wc

rm -rf "$WC"

# --- Build known history: r4 adds NEW, r5 modifies README, r6 deletes NEW. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
echo "hi" > NEW
"$SVN_BIN" add NEW >/dev/null
"$SVN_BIN" commit --author alice --log "add NEW" >/dev/null        # r4

echo "updated-readme" > README
"$SVN_BIN" commit --author alice --log "tweak README" >/dev/null   # r5

"$SVN_BIN" rm NEW >/dev/null
"$SVN_BIN" commit --author alice --log "drop NEW" >/dev/null       # r6

cd /

# --- GET /rev/N/paths endpoint directly: spot-check each classification. ---
r4=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/4/paths")
tlib_check "r4 adds NEW"   "1" "$(echo "$r4" | grep -c '"action":"A","path":"NEW"' || true)"

r5=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/5/paths")
tlib_check "r5 mods README" "1" "$(echo "$r5" | grep -c '"action":"M","path":"README"' || true)"

r6=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/6/paths")
tlib_check "r6 drops NEW"  "1" "$(echo "$r6" | grep -c '"action":"D","path":"NEW"' || true)"

# r0 is the initial empty-copy commit; paths list is empty.
r0=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/0/paths")
tlib_check "r0 empty"      "0" "$(echo "$r0" | grep -c '"action"' || true)"

# r1 adds all seeded paths (README, src, src/main.c, src/lib, src/lib/helper.c).
r1=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/1/paths")
tlib_check "r1 adds README"       "1" "$(echo "$r1" | grep -c '"action":"A","path":"README"' || true)"
tlib_check "r1 adds src/main.c"   "1" "$(echo "$r1" | grep -c '"action":"A","path":"src/main.c"' || true)"

# --- CLI: svn log -v prints "Changed paths:" block with the right actions. ---
out=$("$SVN_BIN" log -v "$URL")
tlib_check "log -v header present"    "7" "$(echo "$out" | grep -c 'Changed paths:' || true)"
tlib_check "log -v shows A NEW"       "1" "$(echo "$out" | grep -c '^   A /NEW$' || true)"
tlib_check "log -v shows M README"    "1" "$(echo "$out" | grep -c '^   M /README$' || true)"
tlib_check "log -v shows D NEW"       "1" "$(echo "$out" | grep -c '^   D /NEW$' || true)"

# Without -v, no "Changed paths:" should appear.
out=$("$SVN_BIN" log "$URL")
tlib_check "bare log no header"       "0" "$(echo "$out" | grep -c 'Changed paths:' || true)"

tlib_summary "test_log_verbose"