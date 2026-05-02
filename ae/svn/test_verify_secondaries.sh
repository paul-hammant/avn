#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

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

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9602}"
REPO=/tmp/svnae_test_sec_repo
WC=/tmp/svnae_test_sec_wc

URL="http://127.0.0.1:$PORT/demo"

# --- (A) Create multi-algo repo. ---
rm -rf "$REPO" "$WC"
"$ADMIN_BIN" create "$REPO" --algos sha256,sha1 >/dev/null
tlib_check "format line"          "svnae-fsfs-1 sha256,sha1"  "$(cat "$REPO/format")"
# First rep (empty blob) should have a 64-char name.
rep_name=$(basename "$(ls "$REPO/reps"/*/*/*.rep | head -1)" .rep)
tlib_check "rep name length"      "64"  "${#rep_name}"

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
tlib_check "hashes has sha256"    "1"  "$(echo "$body" | grep -c '"algo":"sha256"' || true)"
tlib_check "hashes has sha1"      "1"  "$(echo "$body" | grep -c '"algo":"sha1"' || true)"

# --- (C) rep_cache_sec table exists and has entries. ---
secondaries=$(sqlite3 "$REPO/rep-cache.db" "SELECT COUNT(*) FROM rep_cache_sec")
# Expect >0. Exact count depends on dedup: we wrote empty blob (rev 0),
# a rev-0 blob, a root-dir blob, plus note.txt content + new root + rev-1 blob.
# Each unique primary hash gets one secondary row.
[ "$secondaries" -gt 0 ] && echo "  ok   rep_cache_sec non-empty" \
                        || { echo "  FAIL rep_cache_sec empty"; FAILS=$((FAILS+1)); }

# --- (D) Default verify still works. ---
out=$("$SVN_BIN" verify "$URL" --rev 1 2>&1)
tlib_check "default verify OK"    "1"  "$(echo "$out" | grep -c '^verify: OK' || true)"

# --- (E) Verify --secondaries reports files + count. ---
out=$("$SVN_BIN" verify "$URL" --rev 1 --secondaries 2>&1)
tlib_check "secondaries verify OK" "1" "$(echo "$out" | grep -c 'secondary hash(es) verified' || true)"
# Should report at least 1 file (note.txt).
tlib_check "secondaries file count" "1" "$(echo "$out" | grep -c '1 file(s)' || true)"

# --- (F) Tamper: corrupt note.txt's secondary (sha1) hash in rep-cache.db.
#     Default verify should still pass (it checks the primary only).
#     --secondaries verify should detect the mismatch and fail.
tlib_stop_server

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
tlib_check "primary verify still ok" "1" "$(echo "$out" | grep -c '^verify: OK' || true)"

out=$("$SVN_BIN" verify "$URL" --rev 1 --secondaries 2>&1 || true)
# Confirm verify NO LONGER says OK — any of the non-OK exit paths is fine.
tlib_check "secondary tamper detected" "0" "$(echo "$out" | grep -c '^verify: OK' || true)"

tlib_stop_server
rm -rf "$REPO" "$WC"

tlib_summary "test_verify_secondaries"