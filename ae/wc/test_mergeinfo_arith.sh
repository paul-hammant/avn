#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# svn:mergeinfo range arithmetic: cancel, collapse, union.
#
# Scenario: make 5 forward-only commits to src/main.c (r4..r8),
# each inserting a distinct line in its own region so edits don't
# overlap. Then apply merges to a WC at a branch and verify the
# accumulated svn:mergeinfo is in canonical form.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9510}"
REPO=/tmp/svnae_test_miar_repo
WC=/tmp/svnae_test_miar_wc

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_miar_server.log 2>&1 &
SRV=$!
sleep 1.5

# --- Build r4..r8: each adds its own line to src/main.c in a
#     disjoint region so cherry-picks merge3-cleanly. ---
setup=/tmp/svnae_test_miar_setup
rm -rf "$setup"
"$SVN_BIN" checkout "$URL" "$setup" >/dev/null
(cd "$setup" \
  && printf "a1\na2\n"                                     > src/main.c \
  && "$SVN_BIN" commit --author u --log "r4 base"    >/dev/null \
  && printf "a1\na2\nb\n"                                  > src/main.c \
  && "$SVN_BIN" commit --author u --log "r5 add b"   >/dev/null \
  && printf "a1\na2\nb\n\nc\n"                             > src/main.c \
  && "$SVN_BIN" commit --author u --log "r6 add c"   >/dev/null \
  && printf "a1\na2\nb\n\nc\n\nd\n"                        > src/main.c \
  && "$SVN_BIN" commit --author u --log "r7 add d"   >/dev/null \
  && printf "a1\na2\nb\n\nc\n\nd\n\ne\n"                   > src/main.c \
  && "$SVN_BIN" commit --author u --log "r8 add e"   >/dev/null)
rm -rf "$setup"

# --- Case 1: cancel. Apply r5 then reverse r5 — mergeinfo should be empty. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
"$SVN_BIN" update --rev 4 >/dev/null
"$SVN_BIN" merge -c 5  "$URL/src" src >/tmp/miar.out 2>&1
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "after fwd r5"           "src:5-5"        "$mi"
"$SVN_BIN" merge -c -5 "$URL/src" src >/tmp/miar.out 2>&1
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "fwd+rev cancels"        ""               "$mi"
cd /
rm -rf "$WC"

# --- Case 2: adjacent forward ranges collapse. Apply r5 then r6 — result
#     should be one range src:5-6, not two lines. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
"$SVN_BIN" update --rev 4 >/dev/null
"$SVN_BIN" merge -c 5 "$URL/src" src >/tmp/miar.out 2>&1
"$SVN_BIN" merge -c 6 "$URL/src" src >/tmp/miar.out 2>&1
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "r5 + r6 collapses"      "src:5-6"        "$mi"
cd /
rm -rf "$WC"

# --- Case 3: non-adjacent merges stay as a comma-list. Apply r5, then r7
#     — result is src:5-5,7-7 (or 5,7 — we emit full a-b form). ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
"$SVN_BIN" update --rev 4 >/dev/null
"$SVN_BIN" merge -c 5 "$URL/src" src >/tmp/miar.out 2>&1
"$SVN_BIN" merge -c 7 "$URL/src" src >/tmp/miar.out 2>&1
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "non-adjacent kept"      "src:5-5,7-7"    "$mi"

# --- Then apply r6 which bridges the gap — all three collapse to 5-7. ---
"$SVN_BIN" merge -c 6 "$URL/src" src >/tmp/miar.out 2>&1
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "bridge collapses"       "src:5-7"        "$mi"
cd /
rm -rf "$WC"

# --- Case 4: partial cancel — fwd r5..r7 then reverse r6 only. Result:
#     fwd keeps {5,7}, reverse empty (cancelled), serialised as 5-5,7-7. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
"$SVN_BIN" update --rev 4 >/dev/null
"$SVN_BIN" merge -r 4:7 "$URL/src" src >/tmp/miar.out 2>&1
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "range 4:7 merged"       "src:5-7"        "$mi"
"$SVN_BIN" merge -c -6 "$URL/src" src >/tmp/miar.out 2>&1
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
tlib_check "partial cancel r6"      "src:5-5,7-7"    "$mi"

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

tlib_summary "test_mergeinfo_arith"