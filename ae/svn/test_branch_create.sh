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

# Phase 8.2a: svn branch create + spec storage.
#
# Covers:
#   (A) non-super create → 403.
#   (B) super create with includes succeeds; head file + spec on disk.
#   (C) filtered tree: include README only → feat tree has only README.
#   (D) include src/** picks up the whole src subtree (dir + descendants).
#   (E) missing --include → 2 (usage error).
#   (F) duplicate branch name → fails.
#   (G) bad base → fails.
#   (H) svn branch list shows main plus new branches with their globs.
#   (I) /info endpoint exposes the specs map.
#
# 8.2a does NOT yet enforce the spec on commits — that's 8.2b. So we
# only test the creation + listing path here.
set -e
cd "$(dirname "$0")/../.."

AE="$(cd "$(dirname "$0")/../.." && pwd)/.aether_binaries/build/ae"
PORT="${PORT:-9665}"
REPO=/tmp/svnae_test_b82a_repo
SERVER_BIN=/tmp/svnae_test_b82a_server
SEED_BIN=/tmp/svnae_test_b82a_seed
SVN_BIN=/tmp/svnae_test_b82a_svn

TOKEN="b82a-token"
URL="http://127.0.0.1:$PORT/demo"

trap 'pkill -f "${SERVER_BIN} demo" 2>/dev/null || true' EXIT

echo "[*] Build..."
./regen.sh >/dev/null
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" --superuser-token "$TOKEN" >/tmp/svnae_test_b82a_srv.log 2>&1 &
SRV=$!
sleep 1.2

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- (A) non-super create rejected ---
out=$("$SVN_BIN" branch create foo "$URL" --from main --include README 2>&1 || true)
check "non-super refused"  "0" "$(echo "$out" | grep -c 'Created branch' || true)"

# --- (B, C) super-user creates readme-only branch; spec + head on disk ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create readme-only "$URL" \
        --from main --include README 2>&1)
check "readme-only created"      "1" "$(echo "$out" | grep -c 'Created branch' || true)"
check "readme-only/head exists"  "1" "$(test -f "$REPO/branches/readme-only/head" && echo 1 || echo 0)"
check "readme-only/spec body"    "README" "$(cat "$REPO/branches/readme-only/spec")"

# Tree at that rev contains only README.
rev=$(grep -oE 'r[0-9]+' <<< "$out" | grep -oE '[0-9]+')
list=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/$rev/list")
check "readme-only tree has README" "1" "$(echo "$list" | grep -c '"name":"README"' || true)"
check "readme-only tree no src"     "0" "$(echo "$list" | grep -c '"name":"src"' || true)"

# --- (D) include src/** + README → feat has both ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create feat "$URL" \
        --from main --include 'src/**' --include README 2>&1)
check "feat created"  "1" "$(echo "$out" | grep -c 'Created branch' || true)"
rev=$(grep -oE 'r[0-9]+' <<< "$out" | grep -oE '[0-9]+')
list=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/$rev/list")
check "feat tree has README" "1" "$(echo "$list" | grep -c '"name":"README"' || true)"
check "feat tree has src"    "1" "$(echo "$list" | grep -c '"name":"src"' || true)"
list_src=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/$rev/list/src")
check "feat/src has main.c"  "1" "$(echo "$list_src" | grep -c '"name":"main.c"' || true)"

# --- (E) no --include → 2 (usage) ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create bad "$URL" --from main 2>&1 || true)
check "no-include rejected"  "1" "$(echo "$out" | grep -c 'at least one --include' || true)"

# --- (F) duplicate branch name ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create feat "$URL" \
        --from main --include README 2>&1 || true)
check "duplicate refused"  "0" "$(echo "$out" | grep -c 'Created branch' || true)"

# --- (G) unknown base ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create x "$URL" \
        --from no-such --include README 2>&1 || true)
check "bad base refused"   "0" "$(echo "$out" | grep -c 'Created branch' || true)"

# --- (H) svn branch list shows everything with globs ---
out=$("$SVN_BIN" branch list "$URL")
check "list has main"          "1" "$(echo "$out" | grep -c '^main$' || true)"
check "list has readme-only"   "1" "$(echo "$out" | grep -c '^readme-only' || true)"
check "list has feat"          "1" "$(echo "$out" | grep -c '^feat' || true)"
check "list shows README glob" "1" "$(echo "$out" | grep -c '^readme-only.*README' || true)"
check "list shows src/** glob" "1" "$(echo "$out" | grep -c 'src/\*\*' || true)"

# --- (I) /info specs map ---
info=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info")
check "info has specs"         "1" "$(echo "$info" | grep -c '"specs":{' || true)"
check "info specs readme-only" "1" "$(echo "$info" | grep -c '"readme-only":\["README"\]' || true)"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_branch_create: OK"
