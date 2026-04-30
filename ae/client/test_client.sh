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

# End-to-end test for ae/ra: seeds a repo, starts aether-svnserver,
# runs the Aether RA client binary, verifies it completed OK, kills
# the server.
set -e
cd "$(dirname "$0")/../.."

AE="$(cd "$(dirname "$0")/../.." && pwd)/.aether_binaries/build/ae"
PORT="${PORT:-9320}"
REPO=/tmp/svnae_test_ra_repo
SERVER_BIN=/tmp/svnae_test_ra_server
SEED_BIN=/tmp/svnae_test_ra_seed
CLIENT_BIN=/tmp/svnae_test_ra_client

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Building server + seeder + RA client..."
./regen.sh >/dev/null
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"  >/dev/null 2>&1
"$AE" build ae/client/test_client.ae     -o "$CLIENT_BIN" >/dev/null 2>&1

echo "[*] Seeding..."
rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null

echo "[*] Launching server :$PORT ..."
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_ra_server.log 2>&1 &
SRV=$!
sleep 1.5

echo "[*] Running RA test client..."
if "$CLIENT_BIN" "http://127.0.0.1:$PORT" "demo"; then
    rc=0
else
    rc=$?
fi

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO"

if [ "$rc" -ne 0 ]; then
    echo ""
    echo "test_ra: FAIL (client rc=$rc)"
    exit "$rc"
fi
