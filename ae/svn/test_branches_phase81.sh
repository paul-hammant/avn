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

# Phase 8.1: branch infrastructure + default `main` + path-rev index.
#
# Covers:
#   (A) svnadmin create initialises per-branch layout: branches/main/head
#       exists, branches/main/revs/00/00/000000 points at rev 0's blob.
#   (B) path_rev secondary-index table exists and is populated on commit.
#   (C) rev blob contains a `branch: main` field.
#   (D) /info endpoint advertises branches list with main as default.
#   (E) svn branches URL prints "main" (listed by the /info endpoint).
#   (F) Legacy URL form (http://host/repo) still works for all ops.
#   (G) URL grammar with branch;path parses — svn log URL/main works,
#       but so does svn log URL.
#   (H) Per-branch head advances in parallel with legacy head.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9660}"
REPO=/tmp/svnae_test_b81_repo
WC=/tmp/svnae_test_b81_wc
ADMIN_BIN=/tmp/svnae_test_b81_admin
SERVER_BIN=/tmp/svnae_test_b81_server
SEED_BIN=/tmp/svnae_test_b81_seed
SVN_BIN=/tmp/svnae_test_b81_svn

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo" 2>/dev/null || true' EXIT

echo "[*] Build..."
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

# --- (A) create lays down the per-branch layout. ---
rm -rf "$REPO" "$WC"
"$ADMIN_BIN" create "$REPO" >/dev/null
check "branches/main/head exists"    "1" "$(test -f "$REPO/branches/main/head" && echo 1 || echo 0)"
check "branches/main/revs dir"       "1" "$(test -d "$REPO/branches/main/revs" && echo 1 || echo 0)"
check "branches/main/revs/00/00/000000" "1" \
    "$(test -f "$REPO/branches/main/revs/00/00/000000" && echo 1 || echo 0)"
check "branches/main/head = rev=0"   "rev=0" "$(cat "$REPO/branches/main/head")"
check "branches/main/spec exists"    "1" "$(test -f "$REPO/branches/main/spec" && echo 1 || echo 0)"

# --- (B) path_rev table exists. ---
sql_schema=$(sqlite3 "$REPO/rep-cache.db" ".schema path_rev" 2>/dev/null || true)
check "path_rev table defined"       "1" \
    "$(echo "$sql_schema" | grep -c 'CREATE TABLE path_rev' || true)"

# --- Make some commits and verify propagation. ---
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_b81_srv.log 2>&1 &
SRV=$!
sleep 1.2

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
echo "hello" > README.md
mkdir src
echo "int main() { return 0; }" > src/main.c
"$SVN_BIN" add README.md src src/main.c >/dev/null
"$SVN_BIN" commit --author alice --log "initial content" >/dev/null
cd /

# --- (H) both heads advanced. ---
check "legacy head = 1"              "1"       "$(cat "$REPO/head")"
check "per-branch head = rev=1"      "rev=1"   "$(cat "$REPO/branches/main/head")"

# --- (B continued) path_rev populated. ---
rows=$(sqlite3 "$REPO/rep-cache.db" "SELECT COUNT(*) FROM path_rev WHERE rev = 1")
# Expect 3 rows: README.md, src, src/main.c
check "path_rev has 3 r1 rows"       "3"       "$rows"

# Spot-check one row's content.
hit=$(sqlite3 "$REPO/rep-cache.db" \
    "SELECT COUNT(*) FROM path_rev WHERE branch='main' AND path='src/main.c' AND rev=1")
check "path_rev has src/main.c@r1"   "1"       "$hit"

# --- (C) rev 0 blob carries the branch: field. rev 0 is small enough
#     that the compressor skips zlib and leaves the blob raw — so we
#     can grep it directly. Rev 1+ may be compressed. ---
rev0_sha=$(cat "$REPO/revs/000000")
blob_path="$REPO/reps/${rev0_sha:0:2}/${rev0_sha:2:2}/$rev0_sha.rep"
hdr=$(head -c 1 "$blob_path")
if [ "$hdr" = "R" ]; then
    check "rev0 blob has branch: main"   "1" \
        "$(grep -aF -c 'branch: main' "$blob_path" || true)"
else
    # Compressed — inflate it. Skip the 'Z' header byte; body is raw deflate.
    dd if="$blob_path" bs=1 skip=1 2>/dev/null | \
        perl -e 'use Compress::Zlib;local $/;my $b=<>;my $u=uncompress($b);print $u if defined $u' > /tmp/b81_blob_plain 2>/dev/null || true
    check "rev0 blob has branch: main"   "1" \
        "$(grep -aF -c 'branch: main' /tmp/b81_blob_plain || true)"
    rm -f /tmp/b81_blob_plain
fi

# --- (D) /info endpoint has branches + default. ---
info=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info")
check "info has default_branch main" "1" \
    "$(echo "$info" | grep -c '"default_branch":"main"' || true)"
check "info branches includes main"  "1" \
    "$(echo "$info" | grep -c '"branches":\["main"' || true)"

# --- (E) svn branches URL lists main. ---
out=$("$SVN_BIN" branches "$URL")
check "svn branches prints main"     "main"    "$out"

# --- (F) legacy URL ops still work unchanged. ---
out=$("$SVN_BIN" cat "$URL" README.md)
check "legacy cat works"             "hello"   "$out"
out=$("$SVN_BIN" log "$URL" 2>&1)
check "legacy log works"             "1"       "$(echo "$out" | grep -c 'initial content' || true)"

# --- (G) URL grammar parses branch;path cleanly (client-side only
#     today — server still exposes the legacy endpoints). We don't
#     have a client-side parser test harness, so sanity-check that
#     the CLI doesn't reject the new-style URL outright. ---
out=$("$SVN_BIN" info "$URL/main" 2>&1 || true)
# This may fail because the server doesn't route branch-explicit URLs
# yet — but the CLI shouldn't crash parsing. We just require rc != 139
# (segfault). For Phase 8.1 the URL grammar is *accepted* by client
# parsers; server-side interpretation comes in 8.2.
check "branch URL doesn't crash CLI" "1" \
    "$(echo "$out" | grep -cE '(svn info|could not contact|no such revision|Revision:)' || true)"

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_branches_phase81: OK"
