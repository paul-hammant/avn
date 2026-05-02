#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for wc-backed commit.

source "$(dirname "$0")/../../tests/lib.sh"

tlib_use_fixture test_commit
WC=/tmp/svnae_test_wcc_wc


rm -rf "$WC"

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

# --- modify README, add new file, delete tracked, then commit ---
echo "new hello content" > README
echo "new file body"     > NEWFILE
"$SVN_BIN" add NEWFILE   > /dev/null
"$SVN_BIN" rm  src/main.c > /dev/null

out=$("$SVN_BIN" commit --author wc-user --log "WC-backed commit demo")
rev_line=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "commit returns rev 4"   "4" "$rev_line"

# Status should now be empty (everything reconciled).
out=$("$SVN_BIN" status)
tlib_check "clean after commit"     "" "$out"

# Check base_rev was updated in wc.db.
new_base=$(sqlite3 "$WC/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
tlib_check "base_rev now 4"         "4" "$new_base"

# Fetch from server via stateless cat to verify the new state is there.
got=$("$SVN_BIN" cat "$URL" README)
tlib_check "remote README updated"  "new hello content" "$got"

got=$("$SVN_BIN" cat "$URL" NEWFILE)
tlib_check "remote NEWFILE present" "new file body" "$got"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/src/main.c")
tlib_check "remote main.c gone"     "404" "$code"

# Rev 4's metadata.
got=$("$SVN_BIN" info "$URL" --rev 4 | sed -n 's/^Author: *//p')
tlib_check "rev 4 author"           "wc-user" "$got"
got=$("$SVN_BIN" info "$URL" --rev 4 | sed -n 's/^Log: *//p')
tlib_check "rev 4 log"              "WC-backed commit demo" "$got"

# Commit with no changes: clean exit message.
out=$("$SVN_BIN" commit --author foo --log "empty")
tlib_check "no-op commit message"   "No changes to commit." "$out"

cd /

tlib_summary "test_wc_commit"