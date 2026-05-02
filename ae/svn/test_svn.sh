#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for the svn CLI against aether-svnserver.

source "$(dirname "$0")/../../tests/lib.sh"

tlib_use_fixture test_svn

# --- info ---
out=$("$SVN_BIN" info "$URL")
rev_line=$(echo "$out" | awk '/^Revision:/{print $2}')
tlib_check "info head rev"        "3"           "$rev_line"

out=$("$SVN_BIN" info "$URL" --rev 1)
msg_line=$(echo "$out" | sed -n 's/^Log: *//p')
tlib_check "info r1 log"          "first commit" "$msg_line"

# --- log ---
out=$("$SVN_BIN" log "$URL")
# Expect four "r<N> |" lines for revs 0..3, newest first.
first_rev=$(echo "$out" | awk -F'[r ]' '/^r[0-9]/ {print $2; exit}')
tlib_check "log first entry rev"  "3"           "$first_rev"

# --- ls at head ---
out=$("$SVN_BIN" ls "$URL")
# At head (rev 3): README + src/ (LICENSE was dropped in seed's rev 3).
count=$(echo "$out" | wc -l)
tlib_check "ls head count"        "2"           "$count"

# --- ls at rev 1 (has LICENSE) ---
out=$("$SVN_BIN" ls "$URL" --rev 1)
count=$(echo "$out" | wc -l)
tlib_check "ls r1 count"          "3"           "$count"

# --- cat ---
out=$("$SVN_BIN" cat "$URL" --rev 1 README)
tlib_check "cat r1 README"        "Hello"       "$out"

out=$("$SVN_BIN" cat "$URL" --rev 2 src/main.c)
tlib_check "cat r2 main.c"        "int main() { return 42; }" "$out"

# --- commit ---
out=$("$SVN_BIN" commit "$URL" \
       --author cli-user \
       --log "via svn cli" \
       --add-file "NEWFILE=hello from cli" \
       --mkdir dir1)
rev_line=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "commit returns rev"   "4"           "$rev_line"

out=$("$SVN_BIN" info "$URL")
author_line=$(echo "$out" | sed -n 's/^Author: *//p')
tlib_check "r4 author"            "cli-user"    "$author_line"
msg_line=$(echo "$out" | sed -n 's/^Log: *//p')
tlib_check "r4 log"               "via svn cli" "$msg_line"

out=$("$SVN_BIN" cat "$URL" NEWFILE)
tlib_check "cat NEWFILE"          "hello from cli" "$out"

out=$("$SVN_BIN" ls "$URL")
# Now at rev 4: NEWFILE, README, dir1/, src/ — 4 entries.
count=$(echo "$out" | wc -l)
tlib_check "ls after commit count" "4"          "$count"

# --- unknown subcommand ---
if "$SVN_BIN" unknown 2>/dev/null; then
    echo "  FAIL unknown subcommand should nonzero-exit"
    FAILS=$((FAILS + 1))
else
    echo "  ok   unknown subcommand exits nonzero"
fi

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s) failed"
    exit 1
fi
echo ""
echo "test_svn: OK"
