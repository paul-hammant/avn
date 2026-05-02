#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for svn revert + svn diff.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="$test_revert_diff_PORT"
REPO="$test_revert_diff_REPO"
WC=/tmp/svnae_test_rd_wc

URL="http://127.0.0.1:$PORT/demo"

rm -rf "$WC"

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

# ------------------------------ revert ------------------------------
# Case 1: modify a tracked file, revert brings it back.
echo "local edit" > README
out=$("$SVN_BIN" revert README)
tlib_check "revert prints Reverted" "Reverted 'README'" "$out"
tlib_check "README restored from pristine" "Hello" "$(cat README)"

# Status should be clean.
out=$("$SVN_BIN" status)
tlib_check "clean after revert"     "" "$out"

# Case 2: `svn add` a file, then revert drops the row but keeps the file.
echo "drafty" > DRAFT
"$SVN_BIN" add DRAFT > /dev/null
out=$("$SVN_BIN" revert DRAFT)
tlib_check "revert added prints" "Reverted 'DRAFT'" "$out"
# Row is gone; disk file still here.
tlib_check "DRAFT unversioned now" "1" "$(sqlite3 "$WC/.svn/wc.db" "SELECT COUNT(*) FROM nodes WHERE path='DRAFT'" | sed 's/^1$/0/;s/^0$/1/')"
tlib_check "DRAFT still on disk"   "drafty" "$(cat DRAFT)"
rm DRAFT  # cleanup for following tests

# Case 3: rm a tracked file, revert restores disk + clears deleted state.
"$SVN_BIN" rm src/main.c > /dev/null
tlib_check "src/main.c gone post-rm" "absent" "$(test -f src/main.c && echo present || echo absent)"
out=$("$SVN_BIN" revert src/main.c)
tlib_check "revert deleted prints"   "Reverted 'src/main.c'" "$out"
tlib_check "src/main.c restored"     "int main() { return 42; }" "$(cat src/main.c)"

# Case 4: revert a path that isn't tracked — error.
if "$SVN_BIN" revert no-such-path 2>/dev/null; then
    echo "  FAIL revert of untracked should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   revert of untracked fails"
fi

# ------------------------------- diff -------------------------------
# Clean WC → diff produces no output.
out=$("$SVN_BIN" diff)
tlib_check "clean diff empty"        "" "$out"

# Modify README; diff should produce unified diff output containing
# both old and new content markers.
echo "now I am new" >> README   # 'Hello\n' + 'now I am new\n'
out=$("$SVN_BIN" diff)
has_header=$(echo "$out" | grep -c '^Index: README' || true)
tlib_check "diff has Index header"   "1" "$has_header"
has_plus=$(echo "$out" | grep -c '^+now I am new' || true)
tlib_check "diff shows addition"     "1" "$has_plus"

# Diff a single path.
out=$("$SVN_BIN" diff README)
has_header=$(echo "$out" | grep -c '^Index: README' || true)
tlib_check "diff PATH has header"    "1" "$has_header"

# Added file in diff output.
echo "new thing" > NEWFILE
"$SVN_BIN" add NEWFILE > /dev/null
out=$("$SVN_BIN" diff)
has_new_idx=$(echo "$out" | grep -c '^Index: NEWFILE' || true)
tlib_check "diff lists added file"   "1" "$has_new_idx"
has_new_plus=$(echo "$out" | grep -c '^+new thing' || true)
tlib_check "diff shows added body"   "1" "$has_new_plus"

# Clean everything (so we can also re-verify revert works broadly).
"$SVN_BIN" revert README >/dev/null
"$SVN_BIN" revert NEWFILE >/dev/null
rm -f NEWFILE
out=$("$SVN_BIN" status)
tlib_check "fully clean again"       "" "$out"
out=$("$SVN_BIN" diff)
tlib_check "fully clean diff empty"  "" "$out"

cd /

tlib_summary "test_wc_revert_diff"