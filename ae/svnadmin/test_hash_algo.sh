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

# Phase 6.1: pluggable hash algorithms.
#
# Covers:
#   (A) default repo uses sha1 and rep files are 40-char hex
#   (B) svnadmin create --algos sha256 writes a format file recording it
#   (C) rep files under that repo are 64-char hex
#   (D) server's /info advertises the algo
#   (E) full checkout → commit → update cycle works end-to-end
#   (F) unknown algo is rejected by create
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9520}"
ADMIN_BIN=/tmp/svnae_test_hash_admin
SERVER_BIN=/tmp/svnae_test_hash_server
SEED_BIN=/tmp/svnae_test_hash_seed
SVN_BIN=/tmp/svnae_test_hash_svn

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

# --- (A) default (sha1) repo ---
REPO_SHA1=/tmp/svnae_test_hash_repo_sha1
rm -rf "$REPO_SHA1"
"$ADMIN_BIN" create "$REPO_SHA1" >/dev/null
check "sha1 format line"    "svnae-fsfs-1 sha1"   "$(cat "$REPO_SHA1/format")"
# rev 0 writes the empty blob; its name length should be 40.
ls "$REPO_SHA1/reps"/*/*/*.rep >/dev/null 2>&1 || { echo "no rep files"; exit 1; }
rep_name=$(basename "$(ls "$REPO_SHA1/reps"/*/*/*.rep | head -1)" .rep)
check "sha1 rep hex length" "40"                  "${#rep_name}"
rm -rf "$REPO_SHA1"

# --- (B, C) sha256 repo ---
REPO_SHA256=/tmp/svnae_test_hash_repo_sha256
rm -rf "$REPO_SHA256"
"$ADMIN_BIN" create "$REPO_SHA256" --algos sha256 >/dev/null
check "sha256 format line"  "svnae-fsfs-1 sha256" "$(cat "$REPO_SHA256/format")"
rep_name=$(basename "$(ls "$REPO_SHA256/reps"/*/*/*.rep | head -1)" .rep)
check "sha256 rep hex length" "64"                "${#rep_name}"

# --- (D) server /info advertises it ---
"$SERVER_BIN" demo "$REPO_SHA256" "$PORT" >/tmp/svnae_test_hash_srv.log 2>&1 &
SRV=$!
sleep 1.2
info=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info")
check "info has hash_algo=sha256"  "1" \
    "$(echo "$info" | grep -c '"hash_algo":"sha256"' || true)"

# --- (E) full end-to-end round trip on the sha256 repo ---
WC=/tmp/svnae_test_hash_wc
rm -rf "$WC"
"$SVN_BIN" checkout "http://127.0.0.1:$PORT/demo" "$WC" >/dev/null
cd "$WC"
echo "payload on sha256 repo" > NEW.txt
"$SVN_BIN" add NEW.txt >/dev/null
"$SVN_BIN" commit --author alice --log "add NEW on sha256" >/dev/null
out=$("$SVN_BIN" cat "http://127.0.0.1:$PORT/demo" NEW.txt)
check "round-trip content"  "payload on sha256 repo"  "$out"
cd /
rm -rf "$WC"

# Second WC: fresh checkout should pull NEW.txt down again.
"$SVN_BIN" checkout "http://127.0.0.1:$PORT/demo" "$WC" >/dev/null
check "second WC saw NEW"   "payload on sha256 repo"  "$(cat "$WC/NEW.txt")"
cd /
rm -rf "$WC"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO_SHA256"

# --- (F) unknown algo rejected ---
if "$ADMIN_BIN" create /tmp/svnae_test_hash_bad --algos md5 2>/dev/null; then
    echo "  FAIL unknown algo accepted"
    FAILS=$((FAILS+1))
else
    echo "  ok   unknown algo rejected"
fi
rm -rf /tmp/svnae_test_hash_bad

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_hash_algo: OK"
