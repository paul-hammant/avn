#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end: checkout, make local edits, verify svn status output.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9380}"
REPO=/tmp/svnae_test_st_repo
WC=/tmp/svnae_test_st_wc

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null

"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_st_server.log 2>&1 &
SRV=$!
sleep 1.5

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
echo "[*] checked out clean WC"

# --- no changes yet: status should be empty ---
out=$("$SVN_BIN" status "$WC")
tlib_check "clean WC has no status"    ""          "$out"

# --- modify a file ---
echo "different content" > "$WC/README"
out=$("$SVN_BIN" status "$WC")
tlib_check "modified file shows M"     "M       README" "$out"

# --- create an unversioned file ---
echo "new stuff" > "$WC/newfile"
out=$("$SVN_BIN" status "$WC" | sort)
# Sorted alphabetically: M README, ? newfile
# Expected two lines. Check line count plus presence of each.
n=$(echo "$out" | wc -l)
tlib_check "two status lines"          "2"         "$n"
has_mod=$(echo "$out" | grep -c '^M.*README' || true)
tlib_check "M README present"          "1"         "$has_mod"
has_unver=$(echo "$out" | grep -c '^?.*newfile' || true)
tlib_check "? newfile present"         "1"         "$has_unver"

# --- delete a tracked file → Missing (!) ---
rm "$WC/src/main.c"
out=$("$SVN_BIN" status "$WC")
missing_line=$(echo "$out" | grep '!' || true)
tlib_check "missing file shows !"      "!       src/main.c" "$missing_line"

# --- restore the file (un-modify) ---
cat > "$WC/README" <<EOF
Hello
EOF
cat > "$WC/src/main.c" <<EOF
int main() { return 42; }
EOF
rm "$WC/newfile"
out=$("$SVN_BIN" status "$WC")
tlib_check "fully-restored WC clean"   ""          "$out"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

tlib_summary "test_wc_status"