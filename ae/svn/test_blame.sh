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

# Phase 7.6: svn blame.
#
# Covers:
#   (A) every line in a single-commit file is attributed to that rev.
#   (B) an edit to one line updates that line's attribution but leaves
#       the others alone.
#   (C) appending lines attributes only the new lines to the newer rev.
#   (D) inserting lines in the middle correctly attributes the new lines
#       without claiming the surviving lines.
#   (E) --rev N limits to that rev's state.
#   (F) blame on an ACL-denied file returns an error (no leak).
#   (G) aliases: svn annotate / svn praise behave the same.
set -e
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

PORT="${PORT:-9615}"
REPO=/tmp/svnae_test_blame_repo
WC=/tmp/svnae_test_blame_wc
SERVER_BIN="${SERVER_BIN:-$ROOT/target/ae/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/ae/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/ae/svn/bin/svn}"

TOKEN="blame-super-token"
URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} .* ${PORT}" 2>/dev/null || true' EXIT


rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" --superuser-token "$TOKEN" >/tmp/svnae_test_blame_srv.log 2>&1 &
SRV=$!
sleep 1.2

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- Build history: r4 adds file, r5 edits line 2, r6 appends line 4,
#     r7 inserts a new line between line 1 and line 2. ---
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
printf 'line1\nline2\nline3\n' > file.txt
"$SVN_BIN" add file.txt >/dev/null
"$SVN_BIN" commit --author alice --log "r4 initial" >/dev/null    # r4
printf 'line1\nEDITED\nline3\n' > file.txt
"$SVN_BIN" commit --author bob   --log "r5 edit" >/dev/null       # r5
printf 'line1\nEDITED\nline3\nADDED\n' > file.txt
"$SVN_BIN" commit --author carol --log "r6 append" >/dev/null     # r6
printf 'line1\nMIDDLE\nEDITED\nline3\nADDED\n' > file.txt
"$SVN_BIN" commit --author dave  --log "r7 insert" >/dev/null     # r7
cd /

# --- (A/B/C/D) Blame at HEAD (r7). Expected:
#   line1     was added in r4
#   MIDDLE    was added in r7
#   EDITED    was added in r5
#   line3     was added in r4
#   ADDED     was added in r6
out=$("$SVN_BIN" blame "$URL" file.txt)
check "line1 rev"    "4 alice line1"      "$(echo "$out" | sed -n '1p')"
check "MIDDLE rev"   "7 dave MIDDLE"      "$(echo "$out" | sed -n '2p')"
check "EDITED rev"   "5 bob EDITED"       "$(echo "$out" | sed -n '3p')"
check "line3 rev"    "4 alice line3"      "$(echo "$out" | sed -n '4p')"
check "ADDED rev"    "6 carol ADDED"      "$(echo "$out" | sed -n '5p')"

# --- (E) --rev 5 gives us the state after the edit but before the append. ---
out=$("$SVN_BIN" blame "$URL" file.txt --rev 5)
check "r5 line1"     "4 alice line1"      "$(echo "$out" | sed -n '1p')"
check "r5 EDITED"    "5 bob EDITED"       "$(echo "$out" | sed -n '2p')"
check "r5 line3"     "4 alice line3"      "$(echo "$out" | sed -n '3p')"
check "r5 line count" "3"                 "$(echo "$out" | wc -l)"

# --- (F) ACL-denied file. Super-user sets -* on /secret.
#     `svn add` on a dir only adds the dir itself, not its contents —
#     we have to add the file explicitly after.
cd "$WC"
mkdir -p secret
echo "top secret" > secret/plan.txt
"$SVN_BIN" add secret >/dev/null
"$SVN_BIN" add secret/plan.txt >/dev/null
"$SVN_BIN" commit --author admin --log "add secret" >/dev/null
cd /
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" acl set "$URL" secret "-*" "+super" >/dev/null

# Random user sees 404-equivalent, not content.
out=$(SVN_USER=bob "$SVN_BIN" blame "$URL" secret/plan.txt 2>&1 || true)
check "denied blame no content"  "0" "$(echo "$out" | grep -c 'top secret' || true)"

# Super-user can still blame.
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" blame "$URL" secret/plan.txt 2>&1)
check "super-user sees content"  "1" "$(echo "$out" | grep -c 'top secret' || true)"

# --- (G) aliases. ---
out=$("$SVN_BIN" annotate "$URL" file.txt)
check "annotate = blame"  "4 alice line1"  "$(echo "$out" | sed -n '1p')"
out=$("$SVN_BIN" praise "$URL" file.txt)
check "praise = blame"    "4 alice line1"  "$(echo "$out" | sed -n '1p')"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_blame: OK"
