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

# svn log --verbose — per-revision A/M/D path list.
#
# Seeder creates:
#   r0: initial empty commit
#   r1: adds README, src/main.c, src/lib/helper.c   (A ...)
#   r2: modifies src/main.c                         (M src/main.c)
#   r3: further edit
# We then add r4..r6 with known actions to cover A/M/D explicitly.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9500}"
REPO=/tmp/svnae_test_logv_repo
WC=/tmp/svnae_test_logv_wc
SERVER_BIN=/tmp/svnae_test_logv_server
SEED_BIN=/tmp/svnae_test_logv_seed
SVN_BIN=/tmp/svnae_test_logv_svn

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Build..."
./regen.sh >/dev/null
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_logv_server.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- Build known history: r4 adds NEW, r5 modifies README, r6 deletes NEW. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
echo "hi" > NEW
"$SVN_BIN" add NEW >/dev/null
"$SVN_BIN" commit --author alice --log "add NEW" >/dev/null        # r4

echo "updated-readme" > README
"$SVN_BIN" commit --author alice --log "tweak README" >/dev/null   # r5

"$SVN_BIN" rm NEW >/dev/null
"$SVN_BIN" commit --author alice --log "drop NEW" >/dev/null       # r6

cd /

# --- GET /rev/N/paths endpoint directly: spot-check each classification. ---
r4=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/4/paths")
check "r4 adds NEW"   "1" "$(echo "$r4" | grep -c '"action":"A","path":"NEW"' || true)"

r5=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/5/paths")
check "r5 mods README" "1" "$(echo "$r5" | grep -c '"action":"M","path":"README"' || true)"

r6=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/6/paths")
check "r6 drops NEW"  "1" "$(echo "$r6" | grep -c '"action":"D","path":"NEW"' || true)"

# r0 is the initial empty-copy commit; paths list is empty.
r0=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/0/paths")
check "r0 empty"      "0" "$(echo "$r0" | grep -c '"action"' || true)"

# r1 adds all seeded paths (README, src, src/main.c, src/lib, src/lib/helper.c).
r1=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/1/paths")
check "r1 adds README"       "1" "$(echo "$r1" | grep -c '"action":"A","path":"README"' || true)"
check "r1 adds src/main.c"   "1" "$(echo "$r1" | grep -c '"action":"A","path":"src/main.c"' || true)"

# --- CLI: svn log -v prints "Changed paths:" block with the right actions. ---
out=$("$SVN_BIN" log -v "$URL")
check "log -v header present"    "7" "$(echo "$out" | grep -c 'Changed paths:' || true)"
check "log -v shows A NEW"       "1" "$(echo "$out" | grep -c '^   A /NEW$' || true)"
check "log -v shows M README"    "1" "$(echo "$out" | grep -c '^   M /README$' || true)"
check "log -v shows D NEW"       "1" "$(echo "$out" | grep -c '^   D /NEW$' || true)"

# Without -v, no "Changed paths:" should appear.
out=$("$SVN_BIN" log "$URL")
check "bare log no header"       "0" "$(echo "$out" | grep -c 'Changed paths:' || true)"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_log_verbose: OK"
