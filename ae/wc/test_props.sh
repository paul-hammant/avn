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

# svn propset/propget/propdel/proplist.
set -e
cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

PORT="${PORT:-9450}"
REPO=/tmp/svnae_test_pr_repo
WC=/tmp/svnae_test_pr_wc
SERVER_BIN="${SERVER_BIN:-$ROOT/target/ae/svnserver/bin/aether-svnserver}"
SEED_BIN="${SEED_BIN:-$ROOT/target/ae/svnserver/bin/svnae-seed}"
SVN_BIN="${SVN_BIN:-$ROOT/target/ae/svn/bin/svn}"

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT


rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_pr_server.log 2>&1 &
SRV=$!
sleep 1.5
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- propset sets a prop; propget reads it back ---
"$SVN_BIN" propset svn:mime-type text/plain README >/dev/null
check "propget matches"      "text/plain" "$("$SVN_BIN" propget svn:mime-type README)"

# Custom-namespace prop on a dir.
"$SVN_BIN" propset build:label prod src >/dev/null
check "dir prop"             "prod" "$("$SVN_BIN" propget build:label src)"

# --- propset overwrites ---
"$SVN_BIN" propset svn:mime-type application/octet-stream README >/dev/null
check "propget overwritten"  "application/octet-stream" "$("$SVN_BIN" propget svn:mime-type README)"

# --- propget on unset returns nonzero + no output ---
if "$SVN_BIN" propget no-such-prop README 2>/dev/null; then
    echo "  FAIL propget of unset should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   propget of unset fails"
fi

# --- proplist shows all props on a path, sorted ---
"$SVN_BIN" propset another:key thing README >/dev/null
out=$("$SVN_BIN" proplist README)
has_mime=$(echo "$out" | grep -c 'svn:mime-type' || true)
check "proplist shows mime-type"  "1" "$has_mime"
has_another=$(echo "$out" | grep -c 'another:key' || true)
check "proplist shows another"    "1" "$has_another"

# --- propdel removes one ---
"$SVN_BIN" propdel another:key README >/dev/null
out=$("$SVN_BIN" proplist README)
check "proplist sans deleted"     "0" "$(echo "$out" | grep -c 'another:key' || true)"
check "proplist still has mime"   "1" "$(echo "$out" | grep -c 'svn:mime-type' || true)"

# --- propset on untracked path fails ---
echo "draft" > DRAFT
if "$SVN_BIN" propset x y DRAFT 2>/dev/null; then
    echo "  FAIL propset on untracked should fail"
    FAILS=$((FAILS+1))
else
    echo "  ok   propset on untracked fails"
fi
rm DRAFT

# --- proplist on path with no props is empty ---
"$SVN_BIN" propdel svn:mime-type README >/dev/null
out=$("$SVN_BIN" proplist README)
check "proplist empty"       "" "$out"

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
echo "test_wc_props: OK"
