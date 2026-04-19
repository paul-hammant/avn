#!/bin/bash
# End-to-end test for svnadmin: create, populate, dump, load into a
# fresh repo, verify the loaded repo behaves identically (same log,
# same file contents).
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
REPO1=/tmp/svnae_test_adm_repo1
REPO2=/tmp/svnae_test_adm_repo2
DUMP=/tmp/svnae_test_adm.dump
SEED_BIN=/tmp/svnae_test_adm_seed
ADMIN_BIN=/tmp/svnae_test_adm_admin
SERVER_BIN=/tmp/svnae_test_adm_server
SVN_BIN=/tmp/svnae_test_adm_svn
PORT=9430

URL1="http://127.0.0.1:$PORT/r1"
URL2="http://127.0.0.1:$((PORT+1))/r2"

trap 'pkill -f "${SERVER_BIN} .* ${PORT}" 2>/dev/null || true
      pkill -f "${SERVER_BIN} .* $((PORT+1))" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svnadmin/main.ae  -o "$ADMIN_BIN"  >/dev/null 2>&1
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO1" "$REPO2" "$DUMP"

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- create ---
"$ADMIN_BIN" create "$REPO1"
check "format file exists"     "present"           "$(test -f "$REPO1/format" && echo present || echo absent)"
check "format contents"        "svnae-fsfs-1"      "$(cat "$REPO1/format")"
check "initial head is 0"      "0"                 "$(cat "$REPO1/head")"

# --- populate (via seeder, which uses the Phase 3.3 API directly) ---
rm -rf "$REPO1"
"$SEED_BIN" "$REPO1" >/dev/null
n_revs_src=$(ls "$REPO1/revs" | wc -l)
n_reps_src=$(find "$REPO1/reps" -name '*.rep' | wc -l)
check "seeded has 4 revs"      "4"                 "$n_revs_src"

# --- dump ---
"$ADMIN_BIN" dump "$REPO1" --file "$DUMP"
check "dump file exists"       "present"           "$(test -f "$DUMP" && echo present || echo absent)"
check "dump header"            "SVNAE-DUMP 1"      "$(head -1 "$DUMP")"
check "dump ends with END"     "END"               "$(tail -1 "$DUMP")"

# --- load into fresh repo ---
"$ADMIN_BIN" load "$REPO2" --file "$DUMP"
n_revs_dst=$(ls "$REPO2/revs" | wc -l)
n_reps_dst=$(find "$REPO2/reps" -name '*.rep' | wc -l)
check "loaded rev count"       "$n_revs_src"       "$n_revs_dst"
check "loaded rep count"       "$n_reps_src"       "$n_reps_dst"
check "loaded head"            "3"                 "$(cat "$REPO2/head")"

# --- semantic check: run a server on each, compare log + cat outputs ---
"$SERVER_BIN" r1 "$REPO1" "$PORT" > /tmp/adm_s1.log 2>&1 &
S1=$!
"$SERVER_BIN" r2 "$REPO2" "$((PORT+1))" > /tmp/adm_s2.log 2>&1 &
S2=$!
sleep 1.5

log1=$("$SVN_BIN" log "$URL1")
log2=$("$SVN_BIN" log "$URL2")
check "logs match"             "identical"         "$( [ "$log1" = "$log2" ] && echo identical || echo differ)"

for path in README src/main.c docs/README.md; do
    for rev in 1 2 3; do
        # rev 3 drops LICENSE but not these files — they exist at all revs.
        c1=$("$SVN_BIN" cat "$URL1" --rev "$rev" "$path" 2>/dev/null || echo MISSING)
        c2=$("$SVN_BIN" cat "$URL2" --rev "$rev" "$path" 2>/dev/null || echo MISSING)
        check "cat r$rev $path matches" "$c1" "$c2"
    done
done

kill "$S1" "$S2" 2>/dev/null || true
wait "$S1" "$S2" 2>/dev/null || true

# --- corruption detection: truncate the dump, load should fail ---
head -c 100 "$DUMP" > "${DUMP}.trunc"
rm -rf "$REPO2"
if "$ADMIN_BIN" load "$REPO2" --file "${DUMP}.trunc" 2>/dev/null; then
    echo "  FAIL truncated dump should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   truncated dump rejected"
fi
rm -f "${DUMP}.trunc"

# --- create on an existing non-empty dir: we don't enforce this strictly
#     but it shouldn't corrupt what's there. Skip that subtle check
#     for now and verify the happy path only.

rm -rf "$REPO1" "$REPO2" "$DUMP"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_svnadmin: OK"
