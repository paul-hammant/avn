#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Phase 7.3: svn cleanup.
#
# Covers:
#   (A) non-WC path → error, exit 1.
#   (B) clean WC with no stale files → "0 stale file(s) removed".
#   (C) salted stale files under .svn/ and under user tree → removed;
#       real files untouched.
#   (D) wc.db-journal if present → removed.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="$test_cleanup_PORT"
REPO="$test_cleanup_REPO"
WC=/tmp/svnae_test_clean_wc

URL="http://127.0.0.1:$PORT/demo"
rm -rf "$WC"

# --- (A) non-WC path rejected. ---
out=$("$SVN_BIN" cleanup /tmp/definitely_not_a_wc_$$ 2>&1 || true)
tlib_check "non-WC rejected"    "1" "$(echo "$out" | grep -c 'not a working copy' || true)"

# --- (B) fresh WC: nothing stale. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
out=$("$SVN_BIN" cleanup "$WC")
tlib_check "clean WC reports 0"  "1" "$(echo "$out" | grep -c '0 stale' || true)"

# --- (C) salt stale files. ---
mkdir -p "$WC/.svn/pristine/aa/bb"
touch "$WC/.svn/pristine/aa/bb/README.tmp.12345"
touch "$WC/src/main.c.tmp.999"
# Also salt one that looks like but isn't stale — should be untouched.
touch "$WC/README.tmp.not-digits"
out=$("$SVN_BIN" cleanup "$WC")
# Expect two removed: the pristine .tmp file and the src .tmp file.
tlib_check "stale count"        "1" "$(echo "$out" | grep -c '2 stale' || true)"
tlib_check "real tmp kept"      "1" "$(test -f "$WC/README.tmp.not-digits" && echo 1 || echo 0)"
tlib_check "pristine tmp gone"  "0" "$(test -f "$WC/.svn/pristine/aa/bb/README.tmp.12345" && echo 1 || echo 0)"
tlib_check "src tmp gone"       "0" "$(test -f "$WC/src/main.c.tmp.999" && echo 1 || echo 0)"

# --- (D) wc.db-journal removal ---
touch "$WC/.svn/wc.db-journal"
out=$("$SVN_BIN" cleanup "$WC")
tlib_check "journal removed"    "0" "$(test -f "$WC/.svn/wc.db-journal" && echo 1 || echo 0)"
tlib_check "journal counted"    "1" "$(echo "$out" | grep -c '1 stale' || true)"

# Real user files still there.
tlib_check "README survives"    "1" "$(test -f "$WC/README" && echo 1 || echo 0)"
tlib_check "src/main.c survives" "1" "$(test -f "$WC/src/main.c" && echo 1 || echo 0)"

cd /

tlib_summary "test_cleanup"