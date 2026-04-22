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

# Phase 6.2: Merkle verification.
#
# Covers:
#   (A) svn verify URL on a sha1 repo passes for every rev
#   (B) X-Svnae-Node-Hash header is present on cat/list/props responses
#   (C) tampering with a stored .rep file causes verify to fail with -2
#   (D) svn verify on a sha256 repo passes
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9530}"
ADMIN_BIN=/tmp/svnae_verify_admin
SERVER_BIN=/tmp/svnae_verify_server
SEED_BIN=/tmp/svnae_verify_seed
SVN_BIN=/tmp/svnae_verify_svn

trap 'pkill -f "${SERVER_BIN} demo" 2>/dev/null || true' EXIT

echo "[*] Build..."
./regen.sh >/dev/null
"$AE" build ae/svnadmin/main.ae  -o "$ADMIN_BIN"  >/dev/null 2>&1
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- (A) default (sha1) seeded repo — verify all revs. ---
REPO=/tmp/svnae_verify_repo
rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_verify_srv.log 2>&1 &
SRV=$!
sleep 1.2

URL="http://127.0.0.1:$PORT/demo"

# Default (HEAD).
out=$("$SVN_BIN" verify "$URL" 2>&1)
check "verify HEAD prints OK"  "1"  "$(echo "$out" | grep -c '^verify: OK' || true)"

# --rev 1 (smallest non-empty rev in the seed).
out=$("$SVN_BIN" verify "$URL" --rev 1 2>&1)
check "verify r1 prints OK"    "1"  "$(echo "$out" | grep -c '^verify: OK' || true)"

# --- (B) X-Svnae-Node-Hash headers present on cat and list. ---
# Server doesn't handle HEAD requests, so use GET with header dump.
hdrs=$(curl -sD - -o /dev/null "http://127.0.0.1:$PORT/repos/demo/rev/3/cat/src/main.c" 2>&1)
check "cat has Node-Hash"   "1" "$(echo "$hdrs" | grep -ci 'X-Svnae-Node-Hash' || true)"
check "cat has Hash-Algo"   "1" "$(echo "$hdrs" | grep -ci 'X-Svnae-Hash-Algo' || true)"
check "cat has Node-Kind"   "1" "$(echo "$hdrs" | grep -ci 'X-Svnae-Node-Kind' || true)"

hdrs=$(curl -sD - -o /dev/null "http://127.0.0.1:$PORT/repos/demo/rev/3/list/src" 2>&1)
check "list has Node-Hash"  "1" "$(echo "$hdrs" | grep -ci 'X-Svnae-Node-Hash' || true)"

# --- (C) tamper: overwrite a specific raw file-content blob so the
#     server hands back altered bytes but its dir-blob still
#     carries the original sha — the client's re-hash diverges.
#
# Identify src/main.c's content sha via the header route (works
# because that endpoint is live even while the server is running).
sha=$(curl -sD - -o /dev/null "http://127.0.0.1:$PORT/repos/demo/rev/3/cat/src/main.c" \
        | awk 'BEGIN{IGNORECASE=1} /X-Svnae-Node-Hash:/{gsub(/\r/,""); print $2}')
target="$REPO/reps/${sha:0:2}/${sha:2:2}/${sha}.rep"
test -f "$target"
cp "$target" "$target.bak"
# Headers: first byte is 'R' (raw) or 'Z' (zlib). If raw, replace a
# later byte. Z-compressed blobs also fail on corrupt decompress, which
# produces a 404/500 — verify will still return non-zero but not
# necessarily with "MISMATCH" in the message, so force a raw target.
hdr_byte=$(head -c1 "$target")
if [ "$hdr_byte" = "R" ]; then
    printf 'X' | dd of="$target" bs=1 seek=5 count=1 conv=notrunc 2>/dev/null
else
    # Tough luck — flip within the compressed payload. The server will
    # either return a decompress failure (500) or altered bytes.
    printf '\x00' | dd of="$target" bs=1 seek=$(( $(stat -c%s "$target") - 1 )) count=1 conv=notrunc 2>/dev/null
fi
out=$("$SVN_BIN" verify "$URL" 2>&1 || true)
# Either explicit MISMATCH or any non-OK exit is acceptable.
got=0
if echo "$out" | grep -q 'MISMATCH'; then got=1; fi
if ! echo "$out" | grep -q '^verify: OK'; then got=1; fi
check "tamper detected"      "1"  "$got"
# Restore so the server can be cleanly shut down.
mv "$target.bak" "$target"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO"

# --- (D) sha256 repo — same verify pipeline. ---
REPO2=/tmp/svnae_verify_repo_256
rm -rf "$REPO2"
"$ADMIN_BIN" create "$REPO2" --algos sha256 >/dev/null
PORT2=$((PORT + 1))
URL2="http://127.0.0.1:$PORT2/demo"
"$SERVER_BIN" demo "$REPO2" "$PORT2" >/tmp/svnae_verify_srv2.log 2>&1 &
SRV2=$!
sleep 1.2

# Populate with one commit.
WC=/tmp/svnae_verify_wc
rm -rf "$WC"
"$SVN_BIN" checkout "$URL2" "$WC" >/dev/null
cd "$WC"
mkdir dir1
echo "alpha" > dir1/a.txt
echo "beta"  > top.txt
"$SVN_BIN" add dir1 top.txt >/dev/null
"$SVN_BIN" commit --author alice --log "populate" >/dev/null
cd /
rm -rf "$WC"

out=$("$SVN_BIN" verify "$URL2" 2>&1)
check "sha256 verify OK"    "1"  "$(echo "$out" | grep -c '^verify: OK.*sha256' || true)"

kill "$SRV2" 2>/dev/null || true
wait "$SRV2" 2>/dev/null || true
rm -rf "$REPO2"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_verify: OK"
