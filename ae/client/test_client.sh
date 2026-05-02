#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for ae/ra: runs the Aether RA client binary against a
# pre-spawned aether-svnserver fixture, verifies it completed OK.

source "$(dirname "$0")/../../tests/lib.sh"

# Fixture (svn_server "test_client" 9320 in .tests.ae) exports these.
PORT="$test_client_PORT"
CLIENT_BIN="${CLIENT_BIN:-$ROOT/target/ae/client/bin/test_client}"

"$CLIENT_BIN" "http://127.0.0.1:$PORT" "demo"
rc=$?

if [ "$rc" -ne 0 ]; then
    echo ""
    echo "test_ra: FAIL (client rc=$rc)"
    exit "$rc"
fi
