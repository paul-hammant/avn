#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Phase 7.7: svn cp — refuse unless RW-everywhere, and ACL auto-follow.
#
# Covers:
#   (A) alice has RW on src → copy succeeds; src-branch inherits ACL.
#   (B) bob has RW on top but -bob on src/nested → copy src refused (403).
#   (C) chuck with read-only on src → copy refused (403) even though
#       chuck can see the subtree.
#   (D) anonymous refused on any ACL'd subtree.
#   (E) super-user always allowed; ACLs still follow.
#   (F) auto-follow: after alice copies src → src-branch, a user
#       who was denied on src is *also* denied on src-branch. And
#       a user allowed on src is allowed on src-branch.
#   (G) auto-follow handles nested ACLs: ACLs at src/sub get rebased
#       onto src-branch/sub.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9625}"
REPO=/tmp/svnae_test_cpacl_repo
WC=/tmp/svnae_test_cpacl_wc

TOKEN="cpacl-token"
URL="http://127.0.0.1:$PORT/demo"
trap 'pkill -f "${SERVER_BIN} .* ${PORT}" 2>/dev/null || true' EXIT

rm -rf "$REPO" "$WC"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" --superuser-token "$TOKEN" >/tmp/svnae_test_cpacl_srv.log 2>&1 &
SRV=$!
sleep 1.2

# Add a nested directory so we can test recursive subtree ACL checks.
"$SVN_BIN" checkout "$URL" "$WC" >/dev/null
cd "$WC"
mkdir -p src/nested
echo "nested content" > src/nested/note.txt
"$SVN_BIN" add src/nested >/dev/null
"$SVN_BIN" add src/nested/note.txt >/dev/null
"$SVN_BIN" commit --author admin --log "add nested" >/dev/null    # r4
cd /

# --- (A) Set ACL on src: alice=rw, chuck=r, deny everyone else. ---
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" acl set "$URL" src \
    "+alice:rw" "+chuck:r" "-*" >/dev/null                        # r5

# Alice copies src → src-branch. Should succeed.
out=$(SVN_USER=alice "$SVN_BIN" cp "$URL/src" "$URL/src-branch" \
        --author alice --log "alice branches" 2>&1)
tlib_check "alice can branch"       "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (F) Auto-follow: bob is denied on src; he's now also denied on
#     src-branch. The ACL replicated over on commit.
code=$(curl -s -o /dev/null -w '%{http_code}' \
        -H "X-Svnae-User: bob" \
        "http://127.0.0.1:$PORT/repos/demo/rev/6/cat/src-branch/nested/note.txt")
tlib_check "bob denied on src-branch" "404" "$code"

# alice still allowed on src-branch.
code=$(curl -s -o /dev/null -w '%{http_code}' \
        -H "X-Svnae-User: alice" \
        "http://127.0.0.1:$PORT/repos/demo/rev/6/cat/src-branch/nested/note.txt")
tlib_check "alice allowed on branch"  "200" "$code"

# --- (B) bob has RW on top but would need RW on src (he doesn't). ---
out=$(SVN_USER=bob "$SVN_BIN" cp "$URL/src" "$URL/bob-branch" \
        --author bob --log "bob steal" 2>&1 || true)
tlib_check "bob refused"             "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (C) chuck has r on src but not w. Copy must refuse. ---
out=$(SVN_USER=chuck "$SVN_BIN" cp "$URL/src" "$URL/chuck-branch" \
        --author chuck --log "chuck branch" 2>&1 || true)
tlib_check "chuck (r-only) refused"  "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (D) Anonymous refused. ---
out=$("$SVN_BIN" cp "$URL/src" "$URL/anon-branch" \
        --author anon --log "anon branch" 2>&1 || true)
tlib_check "anon refused"            "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (E) Super-user can copy regardless; ACLs still follow. ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" cp "$URL/src" "$URL/super-branch" \
        --author super --log "super branch" 2>&1)
tlib_check "super can branch"        "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# bob still denied on the super's branch copy (ACL followed).
code=$(curl -s -o /dev/null -w '%{http_code}' \
        -H "X-Svnae-User: bob" \
        "http://127.0.0.1:$PORT/repos/demo/rev/7/cat/super-branch/nested/note.txt")
tlib_check "bob denied on super-branch too" "404" "$code"

# --- (G) Nested ACL rebasing. Set a deeper ACL on src/nested allowing
#     only alice, then copy src → src-branch2 and confirm the deeper
#     ACL also rebased. ---
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" acl set "$URL" src/nested \
    "+alice:rw" "-*" >/dev/null                                   # r8

out=$(SVN_USER=alice "$SVN_BIN" cp "$URL/src" "$URL/src-branch2" \
        --author alice --log "branch2" 2>&1)
tlib_check "alice branch2"           "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# Query the /acl endpoint as super-user — we should see rules on
# src-branch2/nested, proving the rebase worked.
rules=$(curl -s -H "X-Svnae-Superuser: $TOKEN" \
             "http://127.0.0.1:$PORT/repos/demo/rev/9/acl/src-branch2/nested")
tlib_check "nested ACL rebased"     "1" "$(echo "$rules" | grep -c '"+alice:rw"' || true)"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" "$WC"

tlib_summary "test_acl_cp_follow"