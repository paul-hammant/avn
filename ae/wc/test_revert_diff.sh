#!/bin/bash
# End-to-end test for svn revert + svn diff.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9420}"
REPO=/tmp/svnae_test_rd_repo
WC=/tmp/svnae_test_rd_wc
SERVER_BIN=/tmp/svnae_test_rd_server
SEED_BIN=/tmp/svnae_test_rd_seed
SVN_BIN=/tmp/svnae_test_rd_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_rd_server.log 2>&1 &
SRV=$!
sleep 1.5

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# ------------------------------ revert ------------------------------
# Case 1: modify a tracked file, revert brings it back.
echo "local edit" > README
out=$("$SVN_BIN" revert README)
check "revert prints Reverted" "Reverted 'README'" "$out"
check "README restored from pristine" "Hello" "$(cat README)"

# Status should be clean.
out=$("$SVN_BIN" status)
check "clean after revert"     "" "$out"

# Case 2: `svn add` a file, then revert drops the row but keeps the file.
echo "drafty" > DRAFT
"$SVN_BIN" add DRAFT > /dev/null
out=$("$SVN_BIN" revert DRAFT)
check "revert added prints" "Reverted 'DRAFT'" "$out"
# Row is gone; disk file still here.
check "DRAFT unversioned now" "1" "$(sqlite3 "$WC/.svn/wc.db" "SELECT COUNT(*) FROM nodes WHERE path='DRAFT'" | sed 's/^1$/0/;s/^0$/1/')"
check "DRAFT still on disk"   "drafty" "$(cat DRAFT)"
rm DRAFT  # cleanup for following tests

# Case 3: rm a tracked file, revert restores disk + clears deleted state.
"$SVN_BIN" rm src/main.c > /dev/null
check "src/main.c gone post-rm" "absent" "$(test -f src/main.c && echo present || echo absent)"
out=$("$SVN_BIN" revert src/main.c)
check "revert deleted prints"   "Reverted 'src/main.c'" "$out"
check "src/main.c restored"     "int main() { return 42; }" "$(cat src/main.c)"

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
check "clean diff empty"        "" "$out"

# Modify README; diff should produce unified diff output containing
# both old and new content markers.
echo "now I am new" >> README   # 'Hello\n' + 'now I am new\n'
out=$("$SVN_BIN" diff)
has_header=$(echo "$out" | grep -c '^Index: README' || true)
check "diff has Index header"   "1" "$has_header"
has_plus=$(echo "$out" | grep -c '^+now I am new' || true)
check "diff shows addition"     "1" "$has_plus"

# Diff a single path.
out=$("$SVN_BIN" diff README)
has_header=$(echo "$out" | grep -c '^Index: README' || true)
check "diff PATH has header"    "1" "$has_header"

# Added file in diff output.
echo "new thing" > NEWFILE
"$SVN_BIN" add NEWFILE > /dev/null
out=$("$SVN_BIN" diff)
has_new_idx=$(echo "$out" | grep -c '^Index: NEWFILE' || true)
check "diff lists added file"   "1" "$has_new_idx"
has_new_plus=$(echo "$out" | grep -c '^+new thing' || true)
check "diff shows added body"   "1" "$has_new_plus"

# Clean everything (so we can also re-verify revert works broadly).
"$SVN_BIN" revert README >/dev/null
"$SVN_BIN" revert NEWFILE >/dev/null
rm -f NEWFILE
out=$("$SVN_BIN" status)
check "fully clean again"       "" "$out"
out=$("$SVN_BIN" diff)
check "fully clean diff empty"  "" "$out"

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_wc_revert_diff: OK"
