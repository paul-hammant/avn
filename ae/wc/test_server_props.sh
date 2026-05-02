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

# Server-side properties: propset on WC1, commit, fresh checkout
# on WC2, propget returns the same values.
set -e
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

PORT="${PORT:-9460}"
REPO=/tmp/svnae_test_sp_repo
WC1=/tmp/svnae_test_sp_wc1
WC2=/tmp/svnae_test_sp_wc2
SERVER_BIN="${SERVER_BIN:-$ROOT/target/ae/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/ae/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/ae/svn/bin/svn}"

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT


rm -rf "$REPO" "$WC1" "$WC2"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_sp_server.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- WC1: check out, propset on a file and a dir, commit. ---
"$SVN_BIN" checkout "$URL" "$WC1" >/dev/null
cd "$WC1"
"$SVN_BIN" propset svn:mime-type text/plain README       >/dev/null
"$SVN_BIN" propset build:label   prod          src       >/dev/null
"$SVN_BIN" propset kv:empty      ""            README    >/dev/null
"$SVN_BIN" commit --author alice --log "set props" >/dev/null
cd /

# --- WC2: fresh checkout. Props must arrive via the server. ---
"$SVN_BIN" checkout "$URL" "$WC2" >/dev/null
cd "$WC2"
check "file prop round-trip"    "text/plain" "$("$SVN_BIN" propget svn:mime-type README)"
check "dir prop round-trip"     "prod"       "$("$SVN_BIN" propget build:label src)"
check "empty-value prop"        ""           "$("$SVN_BIN" propget kv:empty README)"

# --- Second commit from WC1: change one prop, add another, and
#     delete one. WC2 update should observe all three. ---
cd "$WC1"
"$SVN_BIN" propset svn:mime-type application/octet-stream README >/dev/null
"$SVN_BIN" propset owner        alice                    README  >/dev/null
"$SVN_BIN" propdel build:label                           src     >/dev/null
"$SVN_BIN" commit --author alice --log "edit props" >/dev/null
cd /

cd "$WC2"
"$SVN_BIN" update >/dev/null
check "prop change observed"  "application/octet-stream" \
    "$("$SVN_BIN" propget svn:mime-type README)"
check "new prop observed"     "alice" \
    "$("$SVN_BIN" propget owner README)"
# Phase 5.18: propdel on the server now propagates through update.
# WC2 had build:label=prod on src from the first commit; after the
# second commit deleted that prop and WC2 updated, propget should fail.
if "$SVN_BIN" propget build:label src 2>/dev/null; then
    echo "  FAIL propdel did not propagate"
    FAILS=$((FAILS+1))
else
    echo "  ok   propdel propagated"
fi

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC1" "$WC2"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_server_props: OK"
