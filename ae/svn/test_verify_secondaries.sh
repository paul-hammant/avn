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

# Phase 7.5: multi-algo secondary verification.
#
# Covers:
#   (A) create repo with --algos sha256,sha1 → format line is correct,
#       rep file names are sha256-wide (64 hex).
#   (B) /rev/N/hashes/<path> returns primary + secondaries.
#   (C) rep-cache.db has a rep_cache_sec table.
#   (D) `svn verify` default still works (unchanged behaviour).
#   (E) `svn verify --secondaries` walks the tree, counts files and
#       secondary hashes verified.
#   (F) tamper: corrupt a stored secondary hash; `svn verify` passes
#       but `svn verify --secondaries` detects the mismatch.
set -e
cd "$(dirname "$0")/../.."

AE="$(cd "$(dirname "$0")/../.." && pwd)/.aether_binaries/build/ae"
PORT="${PORT:-9602}"
REPO=/tmp/svnae_test_sec_repo
WC=/tmp/svnae_test_sec_wc
ADMIN_BIN=/tmp/svnae_test_sec_admin
SERVER_BIN=/tmp/svnae_test_sec_server
SEED_BIN=/tmp/svnae_test_sec_seed
SVN_BIN=/tmp/svnae_test_sec_svn

URL="http://127.0.0.1:$PORT/demo"

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

# --- (A) Create multi-algo repo. ---
rm -rf "$REPO" "$WC"
"$ADMIN_BIN" create "$REPO" --algos sha256,sha1 >/dev/null
check "format line"          "svnae-fsfs-1 sha256,sha1"  "$(cat "$REPO/format")"
# First rep (empty blob) should have a 64-char name.
rep_name=$(basename "$(ls "$REPO/reps"/*/*/*.rep | head -1)" .rep)
check "rep name length"      "64"  "${#rep_name}"

# Populate with a known file so we have non-empty data to hash.
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_sec_srv.log 2>&1 &
SRV=$!
sleep 1.2
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
echo "hello secondary" > note.txt
"$SVN_BIN" add note.txt >/dev/null
"$SVN_BIN" commit --author alice --log "with content" >/dev/null
cd /

# --- (B) /rev/1/hashes/<path> advertises both hashes. ---
body=$(curl -s "$URL/../repos/demo/rev/1/hashes/note.txt" 2>/dev/null || \
       curl -s "http://127.0.0.1:$PORT/repos/demo/rev/1/hashes/note.txt")
check "hashes has sha256"    "1"  "$(echo "$body" | grep -c '"algo":"sha256"' || true)"
check "hashes has sha1"      "1"  "$(echo "$body" | grep -c '"algo":"sha1"' || true)"

# --- (C) rep_cache_sec table exists and has entries. ---
secondaries=$(sqlite3 "$REPO/rep-cache.db" "SELECT COUNT(*) FROM rep_cache_sec")
# Expect >0. Exact count depends on dedup: we wrote empty blob (rev 0),
# a rev-0 blob, a root-dir blob, plus note.txt content + new root + rev-1 blob.
# Each unique primary hash gets one secondary row.
[ "$secondaries" -gt 0 ] && echo "  ok   rep_cache_sec non-empty" \
                        || { echo "  FAIL rep_cache_sec empty"; FAILS=$((FAILS+1)); }

# --- (D) Default verify still works. ---
out=$("$SVN_BIN" verify "$URL" --rev 1 2>&1)
check "default verify OK"    "1"  "$(echo "$out" | grep -c '^verify: OK' || true)"

# --- (E) Verify --secondaries reports files + count. ---
out=$("$SVN_BIN" verify "$URL" --rev 1 --secondaries 2>&1)
check "secondaries verify OK" "1" "$(echo "$out" | grep -c 'secondary hash(es) verified' || true)"
# Should report at least 1 file (note.txt).
check "secondaries file count" "1" "$(echo "$out" | grep -c '1 file(s)' || true)"

# --- (F) Tamper: corrupt note.txt's secondary (sha1) hash in rep-cache.db.
#     Default verify should still pass (it checks the primary only).
#     --secondaries verify should detect the mismatch and fail.
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true

# Corrupt the secondary hash of note.txt's content blob specifically.
# Ask the server for note.txt's primary hash via the cat endpoint's
# header, then update the matching rep_cache_sec row. Picking a
# specific file-blob guarantees --secondaries actually walks past
# it (LIMIT 1 could hit a rev-blob row, which verify never touches).
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_sec_srv.log 2>&1 &
TMP_SRV=$!
sleep 1.2
note_sha=$(curl -sD - -o /dev/null "$URL/../repos/demo/rev/1/cat/note.txt" 2>/dev/null \
           | awk 'BEGIN{IGNORECASE=1} /X-Svnae-Node-Hash:/{gsub(/\r/,""); print $2}')
if [ -z "$note_sha" ]; then
    # Fallback: try the full URL
    note_sha=$(curl -sD - -o /dev/null "http://127.0.0.1:$PORT/repos/demo/rev/1/cat/note.txt" 2>/dev/null \
               | awk 'BEGIN{IGNORECASE=1} /X-Svnae-Node-Hash:/{gsub(/\r/,""); print $2}')
fi
kill "$TMP_SRV" 2>/dev/null || true
wait "$TMP_SRV" 2>/dev/null || true

sqlite3 "$REPO/rep-cache.db" \
    "UPDATE rep_cache_sec SET secondary_hash='0000000000000000000000000000000000000000' WHERE algo='sha1' AND primary_hash='$note_sha'"

"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_sec_srv.log 2>&1 &
SRV=$!
sleep 1.2

out=$("$SVN_BIN" verify "$URL" --rev 1 2>&1)
check "primary verify still ok" "1" "$(echo "$out" | grep -c '^verify: OK' || true)"

out=$("$SVN_BIN" verify "$URL" --rev 1 --secondaries 2>&1 || true)
# Confirm verify NO LONGER says OK — any of the non-OK exit paths is fine.
check "secondary tamper detected" "0" "$(echo "$out" | grep -c '^verify: OK' || true)"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_verify_secondaries: OK"
