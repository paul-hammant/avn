#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Tests for svn add / rm against a checked-out WC.

source "$(dirname "$0")/../tests/lib.sh"

tlib_use_fixture test_mutate
WC=/tmp/svnae_test_mut_wc


rm -rf "$WC"
tlib_seed "$REPO"
tlib_start_server "$PORT" "$REPO"

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null

cd "$WC"

# --- add a new file ---
echo "hello from cli" > NOTES
out=$("$SVN_BIN" status | sort)
# NOTES shows as '?' before add.
tlib_check "? NOTES pre-add"    "?       NOTES" "$(echo "$out" | grep NOTES)"

"$SVN_BIN" add NOTES > /tmp/mut_add.out
tlib_check "add prints A"       "A         NOTES" "$(cat /tmp/mut_add.out)"

out=$("$SVN_BIN" status | sort)
tlib_check "A NOTES post-add"   "A       NOTES" "$(echo "$out" | grep NOTES)"

# --- rm a previously-committed file ---
"$SVN_BIN" rm README > /tmp/mut_rm.out
tlib_check "rm prints D"        "D         README" "$(cat /tmp/mut_rm.out)"

out=$("$SVN_BIN" status)
tlib_check "README gone on disk" "absent" "$(test -f README && echo present || echo absent)"
tlib_check "D README in status" "1" "$(echo "$out" | grep -c '^D.*README' || true)"

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
tlib_check "DRAFT kept on disk" "present" "$(test -f DRAFT && echo present || echo absent)"
out=$("$SVN_BIN" status)
# After rm of an added (not-yet-committed) file: row dropped → DRAFT is
# unversioned ('?') again.
tlib_check "DRAFT back to ?" "1" "$(echo "$out" | grep -c '^?.*DRAFT' || true)"

cd /

tlib_summary "test_wc_mutate"