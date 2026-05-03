#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Server-side copy = branching. Verifies:
#   - svn cp URL URL creates a new revision whose tree contains the
#     copied subtree at the destination path.
#   - The original tree is untouched.
#   - No new .rep files are created for file content (full subtree
#     rep-sharing via same-sha1 entries).
#   - Both branches can subsequently evolve independently.

source "$(dirname "$0")/../tests/lib.sh"

tlib_use_fixture test_branch
WC=/tmp/svnae_test_br_wc
URL="http://127.0.0.1:$PORT/demo"

rm -rf "$WC"
tlib_seed "$REPO"
tlib_start_server "$PORT" "$REPO"

# Seeder creates head=3. Branch src -> src-branch.
before=$(find "$REPO/reps" -name '*.rep' | wc -l)

out=$("$SVN_BIN" cp "$URL/src" "$URL/src-branch" --author alice --log "branch off src")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "branch commit rev"        "4"                        "$rev"

after=$(find "$REPO/reps" -name '*.rep' | wc -l)
delta=$((after - before))
# Expected: new root-dir blob + new revision blob = 2 new reps.
# The src-branch subtree itself reuses the existing `src` dir blob,
# so the file blobs (main.c, lib/...) are all shared via sha1.
tlib_check "only 2 new reps"          "2"                        "$delta"

# List shows the new branch entry.
out=$("$SVN_BIN" ls "$URL")
tlib_check "ls shows src-branch/"     "1"                        "$(echo "$out" | grep -c 'src-branch/' || true)"

# Contents match between trunk and branch at r4.
a=$("$SVN_BIN" cat "$URL" src/main.c)
b=$("$SVN_BIN" cat "$URL" src-branch/main.c)
tlib_check "branch main.c matches src"   "$a"                   "$b"

# r3 still unchanged — no src-branch there.
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/3/list/src-branch")
tlib_check "r3 has no src-branch"     "404"                      "$code"

# Evolve trunk and branch independently via a checked-out WC.
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
# Modify trunk's main.c
echo "trunk change" > src/main.c
out=$("$SVN_BIN" commit --author alice --log "trunk edit")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "trunk commit"             "5"                        "$rev"

# Branch's main.c should still have the pre-branch content.
b=$("$SVN_BIN" cat "$URL" src-branch/main.c)
tlib_check "branch unaffected by trunk edit" \
                                 "int main() { return 42; }" \
                                 "$b"

# Edit only on the branch.
# We need a WC of the branch portion. For Phase 5.12 our WC checkout is
# always the repo root, so edit via the WC's src-branch/ subpath.
cd "$WC"
echo "branch change" > src-branch/main.c
out=$("$SVN_BIN" commit --author alice --log "branch edit")
rev=$(echo "$out" | awk -F'[ .]' '/^Committed/{print $3}')
tlib_check "branch commit"            "6"                        "$rev"

# Now trunk has "trunk change" (r5) and branch has "branch change" (r6).
a=$("$SVN_BIN" cat "$URL" src/main.c)
b=$("$SVN_BIN" cat "$URL" src-branch/main.c)
tlib_check "trunk final"              "trunk change"             "$a"
tlib_check "branch final"             "branch change"            "$b"
tlib_check "trunk and branch differ"  "differ"                   "$( [ "$a" = "$b" ] && echo same || echo differ)"

cd /

tlib_summary "test_wc_branch"