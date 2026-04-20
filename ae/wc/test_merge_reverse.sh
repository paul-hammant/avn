#!/bin/bash
# svn merge — reverse merge + cherry-pick.
#
# Scenario:
#   seed                     — r0..r3
#   trunk edits r4, r5, r6   — three successive commits on src/main.c
#   Scenarios tested in fresh WCs at HEAD:
#     (A) cherry-pick r5     — only r5's change (line B) applied
#     (B) reverse -c -r5     — r5's change undone; r4+r6 remain
#     (C) reverse -r 6:4     — r6 and r5 both undone (range semantics)
#     (D) mergeinfo format   — reverse range written with leading '-'
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9490}"
REPO=/tmp/svnae_test_rmrg_repo
WC=/tmp/svnae_test_rmrg_wc
SERVER_BIN=/tmp/svnae_test_rmrg_server
SEED_BIN=/tmp/svnae_test_rmrg_seed
SVN_BIN=/tmp/svnae_test_rmrg_svn

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_rmrg_server.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}
check_file() {
    local label="$1" expected="$2" path="$3"
    local tmp=$(mktemp)
    printf "%s" "$expected" > "$tmp"
    if diff -q "$tmp" "$path" >/dev/null 2>&1; then
        echo "  ok   $label"
    else
        echo "  FAIL $label"
        echo "    expected:"; sed 's/^/      /' "$tmp"
        echo "    got:";      sed 's/^/      /' "$path"
        FAILS=$((FAILS+1))
    fi
    rm -f "$tmp"
}

# --- Build history r4, r5, r6 on src/main.c. Each commit edits a
#     different region so that merge3 doesn't treat adjacent line
#     edits as overlapping.
#
#   r4: base content (5 stable lines)
#   r5: inserts "mid-edit" line between line 2 and line 3 (middle region)
#   r6: appends "tail-edit" line after the original content (tail region)
#
# This layout lets us cherry-pick r5, reverse-drop r5, and reverse-range
# r6..r4 with unambiguous merge3 outcomes. ---
setup_wc=/tmp/svnae_test_rmrg_setup
rm -rf "$setup_wc"
"$SVN_BIN" checkout "$URL" "$setup_wc" >/dev/null
R4=$'head 1\nhead 2\nbody 3\ntail 4\ntail 5\n'
R5=$'head 1\nhead 2\nmid-edit\nbody 3\ntail 4\ntail 5\n'
R6=$'head 1\nhead 2\nmid-edit\nbody 3\ntail 4\ntail 5\ntail-edit\n'
(cd "$setup_wc" \
  && printf "%s" "$R4" > src/main.c \
  && "$SVN_BIN" commit --author u --log "r4: base" >/dev/null \
  && printf "%s" "$R5" > src/main.c \
  && "$SVN_BIN" commit --author u --log "r5: mid-edit" >/dev/null \
  && printf "%s" "$R6" > src/main.c \
  && "$SVN_BIN" commit --author u --log "r6: tail-edit" >/dev/null)
rm -rf "$setup_wc"

# After the undo of r5 (reverse cherry-pick), the tree should be:
#   head 1, head 2, body 3, tail 4, tail 5, tail-edit
# i.e. r6 content minus the mid-edit.
R6_UNDO_R5=$'head 1\nhead 2\nbody 3\ntail 4\ntail 5\ntail-edit\n'

# --- (A) Cherry-pick r5 onto a WC at r4. Updates the WC to r4
#     first so the merge source base (r4) matches the local base. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
"$SVN_BIN" update --rev 4 >/dev/null
check_file "pre-cherry r4 content"    "$R4"  src/main.c
"$SVN_BIN" merge -c 5 "$URL/src" src >/tmp/rmrg.out 2>&1
check_file "cherry-pick r5 content"   "$R5"  src/main.c
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
check "cherry mergeinfo"         "src:5-5"                              "$mi"
cd /
rm -rf "$WC"

# --- (B) Reverse cherry-pick -c -5 at HEAD (r6). The WC has r6
#     content; undoing r5 drops the mid-edit line → R6_UNDO_R5. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
check_file "pre-reverse r6 content"   "$R6"          src/main.c
"$SVN_BIN" merge -c -5 "$URL/src" src >/tmp/rmrg.out 2>&1
check_file "reverse -c -5 drops mid"  "$R6_UNDO_R5"  src/main.c
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
check "reverse mergeinfo format" "src:-5-5"                             "$mi"
cd /
rm -rf "$WC"

# --- (C) Reverse range -r 6:4 at HEAD: undoes r5 AND r6, yielding R4. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
"$SVN_BIN" merge -r 6:4 "$URL/src" src >/tmp/rmrg.out 2>&1
check_file "reverse -r 6:4 content"   "$R4"  src/main.c
mi=$("$SVN_BIN" propget svn:mergeinfo . 2>/dev/null || echo "")
check "range reverse mergeinfo"  "src:-5-6"                             "$mi"

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
echo "test_wc_merge_reverse: OK"
