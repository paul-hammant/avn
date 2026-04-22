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

# End-to-end test for wc-backed commit.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9400}"
REPO=/tmp/svnae_test_wcc_repo
WC=/tmp/svnae_test_wcc_wc
SERVER_BIN=/tmp/svnae_test_wcc_server
SEED_BIN=/tmp/svnae_test_wcc_seed
SVN_BIN=/tmp/svnae_test_wcc_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
./regen.sh >/dev/null
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_wcc_server.log 2>&1 &
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

# --- modify README, add new file, delete tracked, then commit ---
echo "new hello content" > README
echo "new file body"     > NEWFILE
"$SVN_BIN" add NEWFILE   > /dev/null
"$SVN_BIN" rm  src/main.c > /dev/null

out=$("$SVN_BIN" commit --author wc-user --log "WC-backed commit demo")
rev_line=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "commit returns rev 4"   "4" "$rev_line"

# Status should now be empty (everything reconciled).
out=$("$SVN_BIN" status)
check "clean after commit"     "" "$out"

# Check base_rev was updated in wc.db.
new_base=$(sqlite3 "$WC/.svn/wc.db" "SELECT value FROM info WHERE key='base_rev'")
check "base_rev now 4"         "4" "$new_base"

# Fetch from server via stateless cat to verify the new state is there.
got=$("$SVN_BIN" cat "$URL" README)
check "remote README updated"  "new hello content" "$got"

got=$("$SVN_BIN" cat "$URL" NEWFILE)
check "remote NEWFILE present" "new file body" "$got"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/src/main.c")
check "remote main.c gone"     "404" "$code"

# Rev 4's metadata.
got=$("$SVN_BIN" info "$URL" --rev 4 | sed -n 's/^Author: *//p')
check "rev 4 author"           "wc-user" "$got"
got=$("$SVN_BIN" info "$URL" --rev 4 | sed -n 's/^Log: *//p')
check "rev 4 log"              "WC-backed commit demo" "$got"

# Commit with no changes: clean exit message.
out=$("$SVN_BIN" commit --author foo --log "empty")
check "no-op commit message"   "No changes to commit." "$out"

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
echo "test_wc_commit: OK"
