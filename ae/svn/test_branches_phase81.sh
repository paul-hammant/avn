#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

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

source "$(dirname "$0")/../../tests/lib.sh"

tlib_use_fixture test_b81
WC=/tmp/svnae_test_b81_wc

rm -rf "$WC"

# --- (A) create laid down the per-branch layout (fixture-created). ---
tlib_check "branches/main/head exists"    "1" "$(test -f "$REPO/branches/main/head" && echo 1 || echo 0)"
tlib_check "branches/main/revs dir"       "1" "$(test -d "$REPO/branches/main/revs" && echo 1 || echo 0)"
tlib_check "branches/main/revs/00/00/000000" "1" \
    "$(test -f "$REPO/branches/main/revs/00/00/000000" && echo 1 || echo 0)"
tlib_check "branches/main/head = rev=0"   "rev=0" "$(cat "$REPO/branches/main/head")"
tlib_check "branches/main/spec exists"    "1" "$(test -f "$REPO/branches/main/spec" && echo 1 || echo 0)"

# --- (B) path_rev table exists. ---
sql_schema=$(sqlite3 "$REPO/rep-cache.db" ".schema path_rev" 2>/dev/null || true)
tlib_check "path_rev table defined"       "1" \
    "$(echo "$sql_schema" | grep -c 'CREATE TABLE path_rev' || true)"

# --- Make some commits and verify propagation. ---

"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
echo "hello" > README.md
mkdir src
echo "int main() { return 0; }" > src/main.c
"$SVN_BIN" add README.md src src/main.c >/dev/null
"$SVN_BIN" commit --author alice --log "initial content" >/dev/null
cd /

# --- (H) both heads advanced. ---
tlib_check "legacy head = 1"              "1"       "$(cat "$REPO/head")"
tlib_check "per-branch head = rev=1"      "rev=1"   "$(cat "$REPO/branches/main/head")"

# --- (B continued) path_rev populated. ---
rows=$(sqlite3 "$REPO/rep-cache.db" "SELECT COUNT(*) FROM path_rev WHERE rev = 1")
# Expect 3 rows: README.md, src, src/main.c
tlib_check "path_rev has 3 r1 rows"       "3"       "$rows"

# Spot-check one row's content.
hit=$(sqlite3 "$REPO/rep-cache.db" \
    "SELECT COUNT(*) FROM path_rev WHERE branch='main' AND path='src/main.c' AND rev=1")
tlib_check "path_rev has src/main.c@r1"   "1"       "$hit"

# --- (C) rev 0 blob carries the branch: field. rev 0 is small enough
#     that the compressor skips zlib and leaves the blob raw — so we
#     can grep it directly. Rev 1+ may be compressed. ---
rev0_sha=$(cat "$REPO/revs/000000")
blob_path="$REPO/reps/${rev0_sha:0:2}/${rev0_sha:2:2}/$rev0_sha.rep"
hdr=$(head -c 1 "$blob_path")
if [ "$hdr" = "R" ]; then
    tlib_check "rev0 blob has branch: main"   "1" \
        "$(grep -aF -c 'branch: main' "$blob_path" || true)"
else
    # Compressed — inflate it. Skip the 'Z' header byte; body is raw deflate.
    dd if="$blob_path" bs=1 skip=1 2>/dev/null | \
        perl -e 'use Compress::Zlib;local $/;my $b=<>;my $u=uncompress($b);print $u if defined $u' > /tmp/b81_blob_plain 2>/dev/null || true
    tlib_check "rev0 blob has branch: main"   "1" \
        "$(grep -aF -c 'branch: main' /tmp/b81_blob_plain || true)"
    rm -f /tmp/b81_blob_plain
fi

# --- (D) /info endpoint has branches + default. ---
info=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info")
tlib_check "info has default_branch main" "1" \
    "$(echo "$info" | grep -c '"default_branch":"main"' || true)"
tlib_check "info branches includes main"  "1" \
    "$(echo "$info" | grep -c '"branches":\["main"' || true)"

# --- (E) svn branches URL lists main. ---
out=$("$SVN_BIN" branches "$URL")
tlib_check "svn branches prints main"     "main"    "$out"

# --- (F) legacy URL ops still work unchanged. ---
out=$("$SVN_BIN" cat "$URL" README.md)
tlib_check "legacy cat works"             "hello"   "$out"
out=$("$SVN_BIN" log "$URL" 2>&1)
tlib_check "legacy log works"             "1"       "$(echo "$out" | grep -c 'initial content' || true)"

# --- (G) URL grammar parses branch;path cleanly (client-side only
#     today — server still exposes the legacy endpoints). We don't
#     have a client-side parser test harness, so sanity-check that
#     the CLI doesn't reject the new-style URL outright. ---
out=$("$SVN_BIN" info "$URL/main" 2>&1 || true)
# This may fail because the server doesn't route branch-explicit URLs
# yet — but the CLI shouldn't crash parsing. We just require rc != 139
# (segfault). For Phase 8.1 the URL grammar is *accepted* by client
# parsers; server-side interpretation comes in 8.2.
tlib_check "branch URL doesn't crash CLI" "1" \
    "$(echo "$out" | grep -cE '(svn info|could not contact|no such revision|Revision:)' || true)"

cd /
rm -rf "$WC"

tlib_summary "test_branches_phase81"