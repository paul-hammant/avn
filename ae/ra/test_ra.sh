#!/bin/bash
# End-to-end test for ae/ra: seeds a repo, starts aether-svnserver,
# runs the Aether RA client binary, verifies it completed OK, kills
# the server.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9320}"
REPO=/tmp/svnae_test_ra_repo
SERVER_BIN=/tmp/svnae_test_ra_server
SEED_BIN=/tmp/svnae_test_ra_seed
CLIENT_BIN=/tmp/svnae_test_ra_client

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Building server + seeder + RA client..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"  >/dev/null 2>&1
"$AE" build ae/ra/test_ra.ae     -o "$CLIENT_BIN" >/dev/null 2>&1

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
