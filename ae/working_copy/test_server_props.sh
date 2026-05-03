#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Server-side properties: propset on WC1, commit, fresh checkout
# on WC2, propget returns the same values.

source "$(dirname "$0")/../../tests/lib.sh"

tlib_use_fixture test_server_props
WC1=/tmp/svnae_test_sp_wc1
WC2=/tmp/svnae_test_sp_wc2

URL="http://127.0.0.1:$PORT/demo"
rm -rf "$WC1" "$WC2"

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
tlib_check "file prop round-trip"    "text/plain" "$("$SVN_BIN" propget svn:mime-type README)"
tlib_check "dir prop round-trip"     "prod"       "$("$SVN_BIN" propget build:label src)"
tlib_check "empty-value prop"        ""           "$("$SVN_BIN" propget kv:empty README)"

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
tlib_check "prop change observed"  "application/octet-stream" \
    "$("$SVN_BIN" propget svn:mime-type README)"
tlib_check "new prop observed"     "alice" \
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

tlib_summary "test_server_props"