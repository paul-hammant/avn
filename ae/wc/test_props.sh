#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# svn propset/propget/propdel/proplist.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9450}"
REPO=/tmp/svnae_test_pr_repo
WC=/tmp/svnae_test_pr_wc

URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_test_pr_server.log 2>&1 &
SRV=$!
sleep 1.5
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"

# --- propset sets a prop; propget reads it back ---
"$SVN_BIN" propset svn:mime-type text/plain README >/dev/null
tlib_check "propget matches"      "text/plain" "$("$SVN_BIN" propget svn:mime-type README)"

# Custom-namespace prop on a dir.
"$SVN_BIN" propset build:label prod src >/dev/null
tlib_check "dir prop"             "prod" "$("$SVN_BIN" propget build:label src)"

# --- propset overwrites ---
"$SVN_BIN" propset svn:mime-type application/octet-stream README >/dev/null
tlib_check "propget overwritten"  "application/octet-stream" "$("$SVN_BIN" propget svn:mime-type README)"

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
tlib_check "proplist shows mime-type"  "1" "$has_mime"
has_another=$(echo "$out" | grep -c 'another:key' || true)
tlib_check "proplist shows another"    "1" "$has_another"

# --- propdel removes one ---
"$SVN_BIN" propdel another:key README >/dev/null
out=$("$SVN_BIN" proplist README)
tlib_check "proplist sans deleted"     "0" "$(echo "$out" | grep -c 'another:key' || true)"
tlib_check "proplist still has mime"   "1" "$(echo "$out" | grep -c 'svn:mime-type' || true)"

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
tlib_check "proplist empty"       "" "$out"

cd /
kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

tlib_summary "test_wc_props"