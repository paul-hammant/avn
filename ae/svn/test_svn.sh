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

# End-to-end test for the svn CLI against aether-svnserver.
set -e
cd "$(dirname "$0")/../.."

AE="$(cd "$(dirname "$0")/../.." && pwd)/.aether_binaries/build/ae"
PORT="${PORT:-9350}"
REPO=/tmp/svnae_test_cli_repo
SERVER_BIN=/tmp/svnae_test_cli_server
SEED_BIN=/tmp/svnae_test_cli_seed
SVN_BIN=/tmp/svnae_test_cli_svn

URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Building server + seeder + svn CLI..."
./regen.sh >/dev/null
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

echo "[*] Seeding..."
rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null

echo "[*] Launching server :$PORT ..."
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_cli_server.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  ok   $label"
    else
        echo "  FAIL $label"
        echo "       expected: $(echo "$expected" | head -c 200)"
        echo "       got     : $(echo "$actual" | head -c 200)"
        FAILS=$((FAILS + 1))
    fi
}

# --- info ---
out=$("$SVN_BIN" info "$URL")
rev_line=$(echo "$out" | awk '/^Revision:/{print $2}')
check "info head rev"        "3"           "$rev_line"

out=$("$SVN_BIN" info "$URL" --rev 1)
msg_line=$(echo "$out" | sed -n 's/^Log: *//p')
check "info r1 log"          "first commit" "$msg_line"

# --- log ---
out=$("$SVN_BIN" log "$URL")
# Expect four "r<N> |" lines for revs 0..3, newest first.
first_rev=$(echo "$out" | awk -F'[r ]' '/^r[0-9]/ {print $2; exit}')
check "log first entry rev"  "3"           "$first_rev"

# --- ls at head ---
out=$("$SVN_BIN" ls "$URL")
# At head (rev 3): README + src/ (LICENSE was dropped in seed's rev 3).
count=$(echo "$out" | wc -l)
check "ls head count"        "2"           "$count"

# --- ls at rev 1 (has LICENSE) ---
out=$("$SVN_BIN" ls "$URL" --rev 1)
count=$(echo "$out" | wc -l)
check "ls r1 count"          "3"           "$count"

# --- cat ---
out=$("$SVN_BIN" cat "$URL" --rev 1 README)
check "cat r1 README"        "Hello"       "$out"

out=$("$SVN_BIN" cat "$URL" --rev 2 src/main.c)
check "cat r2 main.c"        "int main() { return 42; }" "$out"

# --- commit ---
out=$("$SVN_BIN" commit "$URL" \
       --author cli-user \
       --log "via svn cli" \
       --add-file "NEWFILE=hello from cli" \
       --mkdir dir1)
rev_line=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
check "commit returns rev"   "4"           "$rev_line"

out=$("$SVN_BIN" info "$URL")
author_line=$(echo "$out" | sed -n 's/^Author: *//p')
check "r4 author"            "cli-user"    "$author_line"
msg_line=$(echo "$out" | sed -n 's/^Log: *//p')
check "r4 log"               "via svn cli" "$msg_line"

out=$("$SVN_BIN" cat "$URL" NEWFILE)
check "cat NEWFILE"          "hello from cli" "$out"

out=$("$SVN_BIN" ls "$URL")
# Now at rev 4: NEWFILE, README, dir1/, src/ — 4 entries.
count=$(echo "$out" | wc -l)
check "ls after commit count" "4"          "$count"

# --- unknown subcommand ---
if "$SVN_BIN" unknown 2>/dev/null; then
    echo "  FAIL unknown subcommand should nonzero-exit"
    FAILS=$((FAILS + 1))
else
    echo "  ok   unknown subcommand exits nonzero"
fi

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s) failed"
    exit 1
fi
echo ""
echo "test_svn: OK"
