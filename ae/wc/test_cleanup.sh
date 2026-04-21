#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Portions copyright Apache Subversion project contributors (2001-2026).
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
# implied. See the License for the specific language governing
# permissions and limitations under the License.

# Phase 7.3: svn cleanup.
#
# Covers:
#   (A) non-WC path → error, exit 1.
#   (B) clean WC with no stale files → "0 stale file(s) removed".
#   (C) salted stale files under .svn/ and under user tree → removed;
#       real files untouched.
#   (D) wc.db-journal if present → removed.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9560}"
REPO=/tmp/svnae_test_clean_repo
WC=/tmp/svnae_test_clean_wc
SERVER_BIN=/tmp/svnae_test_clean_server
SEED_BIN=/tmp/svnae_test_clean_seed
SVN_BIN=/tmp/svnae_test_clean_svn

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_clean_srv.log 2>&1 &
SRV=$!
sleep 1.2

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- (A) non-WC path rejected. ---
out=$("$SVN_BIN" cleanup /tmp/definitely_not_a_wc_$$ 2>&1 || true)
check "non-WC rejected"    "1" "$(echo "$out" | grep -c 'not a working copy' || true)"

# --- (B) fresh WC: nothing stale. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
out=$("$SVN_BIN" cleanup "$WC")
check "clean WC reports 0"  "1" "$(echo "$out" | grep -c '0 stale' || true)"

# --- (C) salt stale files. ---
mkdir -p "$WC/.svn/pristine/aa/bb"
touch "$WC/.svn/pristine/aa/bb/README.tmp.12345"
touch "$WC/src/main.c.tmp.999"
# Also salt one that looks like but isn't stale — should be untouched.
touch "$WC/README.tmp.not-digits"
out=$("$SVN_BIN" cleanup "$WC")
# Expect two removed: the pristine .tmp file and the src .tmp file.
check "stale count"        "1" "$(echo "$out" | grep -c '2 stale' || true)"
check "real tmp kept"      "1" "$(test -f "$WC/README.tmp.not-digits" && echo 1 || echo 0)"
check "pristine tmp gone"  "0" "$(test -f "$WC/.svn/pristine/aa/bb/README.tmp.12345" && echo 1 || echo 0)"
check "src tmp gone"       "0" "$(test -f "$WC/src/main.c.tmp.999" && echo 1 || echo 0)"

# --- (D) wc.db-journal removal ---
touch "$WC/.svn/wc.db-journal"
out=$("$SVN_BIN" cleanup "$WC")
check "journal removed"    "0" "$(test -f "$WC/.svn/wc.db-journal" && echo 1 || echo 0)"
check "journal counted"    "1" "$(echo "$out" | grep -c '1 stale' || true)"

# Real user files still there.
check "README survives"    "1" "$(test -f "$WC/README" && echo 1 || echo 0)"
check "src/main.c survives" "1" "$(test -f "$WC/src/main.c" && echo 1 || echo 0)"

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
echo "test_cleanup: OK"
