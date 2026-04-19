#!/bin/bash
# End-to-end: checkout, make local edits, verify svn status output.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9380}"
REPO=/tmp/svnae_test_st_repo
WC=/tmp/svnae_test_st_wc
SERVER_BIN=/tmp/svnae_test_st_server
SEED_BIN=/tmp/svnae_test_st_seed
SVN_BIN=/tmp/svnae_test_st_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null

"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_st_server.log 2>&1 &
SRV=$!
sleep 1.5

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
echo "[*] checked out clean WC"

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- no changes yet: status should be empty ---
out=$("$SVN_BIN" status "$WC")
check "clean WC has no status"    ""          "$out"

# --- modify a file ---
echo "different content" > "$WC/README"
out=$("$SVN_BIN" status "$WC")
check "modified file shows M"     "M       README" "$out"

# --- create an unversioned file ---
echo "new stuff" > "$WC/newfile"
out=$("$SVN_BIN" status "$WC" | sort)
# Sorted alphabetically: M README, ? newfile
# Expected two lines. Check line count plus presence of each.
n=$(echo "$out" | wc -l)
check "two status lines"          "2"         "$n"
has_mod=$(echo "$out" | grep -c '^M.*README' || true)
check "M README present"          "1"         "$has_mod"
has_unver=$(echo "$out" | grep -c '^?.*newfile' || true)
check "? newfile present"         "1"         "$has_unver"

# --- delete a tracked file → Missing (!) ---
rm "$WC/src/main.c"
out=$("$SVN_BIN" status "$WC")
missing_line=$(echo "$out" | grep '!' || true)
check "missing file shows !"      "!       src/main.c" "$missing_line"

# --- restore the file (un-modify) ---
cat > "$WC/README" <<EOF
Hello
EOF
cat > "$WC/src/main.c" <<EOF
int main() { return 42; }
EOF
rm "$WC/newfile"
out=$("$SVN_BIN" status "$WC")
check "fully-restored WC clean"   ""          "$out"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_wc_status: OK"
