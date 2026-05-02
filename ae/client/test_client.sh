#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Portions copyright Apache Subversion project contributors (2001-2026).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for ae/ra: seeds a repo, starts aether-svnserver,
# runs the Aether RA client binary, verifies it completed OK, kills
# the server.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9320}"
REPO=/tmp/svnae_test_ra_repo
CLIENT_BIN="${CLIENT_BIN:-$ROOT/target/ae/client/bin/test_client}"

echo "[*] Seeding..."
rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null

echo "[*] Launching server :$PORT ..."
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_ra_server.log 2>&1 &
SRV=$!
sleep 1.5

echo "[*] Running RA test client..."
"$CLIENT_BIN" "http://127.0.0.1:$PORT" "demo"
rc=$?

tlib_stop_server
rm -rf "$REPO"

if [ "$rc" -ne 0 ]; then
    echo ""
    echo "test_ra: FAIL (client rc=$rc)"
    exit "$rc"
fi
