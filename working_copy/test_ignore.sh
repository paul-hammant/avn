#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# svn:ignore: svn status should skip '?' entries whose name matches any
# glob in the parent dir's svn:ignore property.

source "$(dirname "$0")/../tests/lib.sh"

tlib_use_fixture test_ignore
WC=/tmp/svnae_test_ig_wc

rm -rf "$WC"
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

# src/ exists in the seed. Create unversioned files under it.
echo "a" > src/something.o
echo "b" > src/other.o
echo "c" > src/notes.txt

# Before setting svn:ignore: all three show as '?' entries.
out=$("$SVN_BIN" status)
n_q=$(echo "$out" | grep -c '^?' || true)
tlib_check "3 unversioned before ignore"   "3" "$n_q"

# Set svn:ignore on src/ to skip *.o
"$SVN_BIN" propset svn:ignore '*.o' src >/dev/null

# Now status should only show notes.txt.
out=$("$SVN_BIN" status)
n_q=$(echo "$out" | grep -c '^?' || true)
tlib_check "1 unversioned after *.o"        "1" "$n_q"
notes=$(echo "$out" | grep notes.txt | awk '{print $2}')
tlib_check "notes.txt still listed"         "src/notes.txt" "$notes"

# Multi-pattern svn:ignore
"$SVN_BIN" propset svn:ignore $'*.o\nnotes.txt' src >/dev/null
out=$("$SVN_BIN" status)
n_q=$(echo "$out" | grep -c '^?' || true)
tlib_check "0 unversioned with both patterns" "0" "$n_q"

# Empty patterns / blank lines are tolerated.
"$SVN_BIN" propset svn:ignore $'\n*.o\n\n' src >/dev/null
out=$("$SVN_BIN" status)
has_notes=$(echo "$out" | grep -c '^?.*notes.txt' || true)
tlib_check "blank lines tolerated"          "1" "$has_notes"

# svn:ignore only applies to immediate children — nested dirs are a
# separate ignore scope. Verify that files directly under src/ with
# a *.o name are hidden even when svn:ignore is set at root too.
echo "x" > top-ignore-me.o
# No svn:ignore at root — top-ignore-me.o still visible.
out=$("$SVN_BIN" status)
tlib_check "root *.o still visible (no ignore at root)" "1" "$(echo "$out" | grep -c 'top-ignore-me.o' || true)"

rm top-ignore-me.o src/something.o src/other.o src/notes.txt
"$SVN_BIN" propdel svn:ignore src >/dev/null
out=$("$SVN_BIN" status)
tlib_check "clean after cleanup"            "" "$out"

cd /

tlib_summary "test_wc_ignore"