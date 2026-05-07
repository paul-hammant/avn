#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for avnadmin: create, populate, dump, load into a
# fresh repo, verify the loaded repo behaves identically (same log,
# same file contents).

source "$(dirname "$0")/../tests/lib.sh"

REPO1=/tmp/avn_test_adm_repo1
REPO2=/tmp/avn_test_adm_repo2
DUMP=/tmp/avn_test_adm.dump
PORT=9430

URL1="http://127.0.0.1:$PORT/r1"
URL2="http://127.0.0.1:$((PORT+1))/r2"

trap 'pkill -f "${SERVER_BIN} .* ${PORT}" 2>/dev/null || true
      pkill -f "${SERVER_BIN} .* $((PORT+1))" 2>/dev/null || true' EXIT

rm -rf "$REPO1" "$REPO2" "$DUMP"

# --- create ---
"$ADMIN_BIN" create "$REPO1"
tlib_check "format file exists"     "present"           "$(test -f "$REPO1/format" && echo present || echo absent)"
tlib_check "format contents"        "svnae-fsfs-1 sha1" "$(cat "$REPO1/format")"
tlib_check "initial head is 0"      "0"                 "$(cat "$REPO1/head")"

# --- populate (via seeder, which uses the Phase 3.3 API directly) ---
rm -rf "$REPO1"
"$SEED_BIN" "$REPO1" >/dev/null
n_revs_src=$(ls "$REPO1/revs" | wc -l)
n_reps_src=$(find "$REPO1/reps" -name '*.rep' | wc -l)
tlib_check "seeded has 4 revs"      "4"                 "$n_revs_src"

# --- dump ---
"$ADMIN_BIN" dump "$REPO1" --file "$DUMP"
tlib_check "dump file exists"       "present"           "$(test -f "$DUMP" && echo present || echo absent)"
tlib_check "dump header"            "SVNAE-DUMP 1"      "$(head -1 "$DUMP")"
tlib_check "dump ends with END"     "END"               "$(tail -1 "$DUMP")"

# --- load into fresh repo ---
"$ADMIN_BIN" load "$REPO2" --file "$DUMP"
n_revs_dst=$(ls "$REPO2/revs" | wc -l)
n_reps_dst=$(find "$REPO2/reps" -name '*.rep' | wc -l)
tlib_check "loaded rev count"       "$n_revs_src"       "$n_revs_dst"
tlib_check "loaded rep count"       "$n_reps_src"       "$n_reps_dst"
tlib_check "loaded head"            "3"                 "$(cat "$REPO2/head")"

# --- semantic check: run a server on each, compare log + cat outputs ---
"$SERVER_BIN" r1 "$REPO1" "$PORT" > /tmp/adm_s1.log 2>&1 &
S1=$!
"$SERVER_BIN" r2 "$REPO2" "$((PORT+1))" > /tmp/adm_s2.log 2>&1 &
S2=$!
sleep 1.5

log1=$("$AVN_BIN" log "$URL1")
log2=$("$AVN_BIN" log "$URL2")
tlib_check "logs match"             "identical"         "$( [ "$log1" = "$log2" ] && echo identical || echo differ)"

for path in README src/main.c docs/README.md; do
    for rev in 1 2 3; do
        # rev 3 drops LICENSE but not these files — they exist at all revs.
        c1=$("$AVN_BIN" cat "$URL1" --rev "$rev" "$path" 2>/dev/null || echo MISSING)
        c2=$("$AVN_BIN" cat "$URL2" --rev "$rev" "$path" 2>/dev/null || echo MISSING)
        tlib_check "cat r$rev $path matches" "$c1" "$c2"
    done
done

kill "$S1" "$S2" 2>/dev/null || true
wait "$S1" "$S2" 2>/dev/null || true

# --- corruption detection: truncate the dump, load should fail ---
head -c 100 "$DUMP" > "${DUMP}.trunc"
rm -rf "$REPO2"
if "$ADMIN_BIN" load "$REPO2" --file "${DUMP}.trunc" 2>/dev/null; then
    echo "  FAIL truncated dump should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   truncated dump rejected"
fi
rm -f "${DUMP}.trunc"

# --- create on an existing non-empty dir: we don't enforce this strictly
#     but it shouldn't corrupt what's there. Skip that subtle check
#     for now and verify the happy path only.

rm -rf "$REPO1" "$REPO2" "$DUMP"

tlib_summary "test_avnadmin"