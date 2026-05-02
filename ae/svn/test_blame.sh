#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Phase 7.6: svn blame.
#
# Covers:
#   (A) every line in a single-commit file is attributed to that rev.
#   (B) an edit to one line updates that line's attribution but leaves
#       the others alone.
#   (C) appending lines attributes only the new lines to the newer rev.
#   (D) inserting lines in the middle correctly attributes the new lines
#       without claiming the surviving lines.
#   (E) --rev N limits to that rev's state.
#   (F) blame on an ACL-denied file returns an error (no leak).
#   (G) aliases: svn annotate / svn praise behave the same.

source "$(dirname "$0")/../../tests/lib.sh"

tlib_use_fixture test_blame
WC=/tmp/svnae_test_blame_wc

TOKEN="blame-super-token"
URL="http://127.0.0.1:$PORT/demo"
rm -rf "$WC"

# --- Build history: r4 adds file, r5 edits line 2, r6 appends line 4,
#     r7 inserts a new line between line 1 and line 2. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
printf 'line1\nline2\nline3\n' > file.txt
"$SVN_BIN" add file.txt >/dev/null
"$SVN_BIN" commit --author alice --log "r4 initial" >/dev/null    # r4
printf 'line1\nEDITED\nline3\n' > file.txt
"$SVN_BIN" commit --author bob   --log "r5 edit" >/dev/null       # r5
printf 'line1\nEDITED\nline3\nADDED\n' > file.txt
"$SVN_BIN" commit --author carol --log "r6 append" >/dev/null     # r6
printf 'line1\nMIDDLE\nEDITED\nline3\nADDED\n' > file.txt
"$SVN_BIN" commit --author dave  --log "r7 insert" >/dev/null     # r7
cd /

# --- (A/B/C/D) Blame at HEAD (r7). Expected:
#   line1     was added in r4
#   MIDDLE    was added in r7
#   EDITED    was added in r5
#   line3     was added in r4
#   ADDED     was added in r6
out=$("$SVN_BIN" blame "$URL" file.txt)
tlib_check "line1 rev"    "4 alice line1"      "$(echo "$out" | sed -n '1p')"
tlib_check "MIDDLE rev"   "7 dave MIDDLE"      "$(echo "$out" | sed -n '2p')"
tlib_check "EDITED rev"   "5 bob EDITED"       "$(echo "$out" | sed -n '3p')"
tlib_check "line3 rev"    "4 alice line3"      "$(echo "$out" | sed -n '4p')"
tlib_check "ADDED rev"    "6 carol ADDED"      "$(echo "$out" | sed -n '5p')"

# --- (E) --rev 5 gives us the state after the edit but before the append. ---
out=$("$SVN_BIN" blame "$URL" file.txt --rev 5)
tlib_check "r5 line1"     "4 alice line1"      "$(echo "$out" | sed -n '1p')"
tlib_check "r5 EDITED"    "5 bob EDITED"       "$(echo "$out" | sed -n '2p')"
tlib_check "r5 line3"     "4 alice line3"      "$(echo "$out" | sed -n '3p')"
tlib_check "r5 line count" "3"                 "$(echo "$out" | wc -l)"

# --- (F) ACL-denied file. Super-user sets -* on /secret.
#     `svn add` on a dir only adds the dir itself, not its contents —
#     we have to add the file explicitly after.
cd "$WC"
mkdir -p secret
echo "top secret" > secret/plan.txt
"$SVN_BIN" add secret >/dev/null
"$SVN_BIN" add secret/plan.txt >/dev/null
"$SVN_BIN" commit --author admin --log "add secret" >/dev/null
cd /
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" acl set "$URL" secret "-*" "+super" >/dev/null

# Random user sees 404-equivalent, not content.
out=$(SVN_USER=bob "$SVN_BIN" blame "$URL" secret/plan.txt 2>&1 || true)
tlib_check "denied blame no content"  "0" "$(echo "$out" | grep -c 'top secret' || true)"

# Super-user can still blame.
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" blame "$URL" secret/plan.txt 2>&1)
tlib_check "super-user sees content"  "1" "$(echo "$out" | grep -c 'top secret' || true)"

# --- (G) aliases. ---
out=$("$SVN_BIN" annotate "$URL" file.txt)
tlib_check "annotate = blame"  "4 alice line1"  "$(echo "$out" | sed -n '1p')"
out=$("$SVN_BIN" praise "$URL" file.txt)
tlib_check "praise = blame"    "4 alice line1"  "$(echo "$out" | sed -n '1p')"

tlib_summary "test_blame"