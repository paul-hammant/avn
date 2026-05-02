#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Phase 7.2: write-side ACL enforcement + copy guard.
#
# Covers:
#   (A) super-user sets ACL allowing alice:rw on src, denying others.
#   (B) alice can commit inside src.
#   (C) bob (denied) cannot commit to src — server returns 403.
#   (D) r/w distinction: chuck has +chuck:r (read-only). Chuck sees
#       src but his commit to src is refused.
#   (E) anonymous user (no SVN_USER) is refused on write.
#   (F) cannot self-elevate: alice tries to set +bob on /src without
#       write permission on a different path — well here alice has
#       write on /src so that's fine. Test: bob (no write) tries to
#       set +bob on /src — must be refused.
#   (G) super-user always bypasses.

source "$(dirname "$0")/../../tests/lib.sh"

PORT="${PORT:-9550}"

TOKEN="test-super-token-7-2"

REPO=/tmp/svnae_test_aclw_repo
tlib_seed "$REPO"
tlib_start_server "$PORT" "$REPO" demo --superuser-token "$TOKEN"

URL="http://127.0.0.1:$PORT/demo"

# --- (A) Super-user sets ACL: alice:rw, chuck:r, everyone else denied.
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" acl set "$URL" src \
    "+alice:rw" "+chuck:r" "-*" >/dev/null

# --- (B) alice can commit in src. Uses the URL-flag commit path
#     (no WC needed) to keep the test tight.
out=$(SVN_USER=alice "$SVN_BIN" commit "$URL" \
        --author alice --log "alice adds file" \
        --add-file 'src/alice.txt=hello' 2>&1)
tlib_check "alice commits in src"  "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (C) bob (denied) cannot commit to src. Server returns 403.
out=$(SVN_USER=bob "$SVN_BIN" commit "$URL" \
        --author bob --log "bob tries" \
        --add-file 'src/bob.txt=nope' 2>&1 || true)
tlib_check "bob refused at src"    "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (D) chuck is read-only: his commit is refused.
out=$(SVN_USER=chuck "$SVN_BIN" commit "$URL" \
        --author chuck --log "chuck tries" \
        --add-file 'src/chuck.txt=readonly' 2>&1 || true)
tlib_check "chuck (r-only) refused" "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# ... but chuck can still READ src (confirms rw vs r distinction).
cat_chuck=$(SVN_USER=chuck "$SVN_BIN" cat "$URL" src/alice.txt)
tlib_check "chuck can read alice's file" "hello" "$cat_chuck"

# --- (E) anonymous (no user header) is refused on write.
out=$("$SVN_BIN" commit "$URL" \
        --author anon --log "anon tries" \
        --add-file 'src/anon.txt=noname' 2>&1 || true)
tlib_check "anon refused at src"   "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# Anon can commit OUTSIDE src (root has no ACL restricting it).
out=$("$SVN_BIN" commit "$URL" \
        --author anon --log "anon outside src" \
        --add-file 'public.txt=ok' 2>&1)
tlib_check "anon commits at root"  "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (F) self-elevation refused: bob tries to set +bob:rw on /src.
#     Server must refuse because bob has no write on /src.
out=$(SVN_USER=bob "$SVN_BIN" acl set "$URL" src "+bob:rw" 2>&1 || true)
tlib_check "bob can't self-elevate"  "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (G) super-user bypasses everything.
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" commit "$URL" \
        --author super --log "super in src" \
        --add-file 'src/super.txt=ok' 2>&1)
tlib_check "super-user commits in src" "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- Copy guard: bob tries to copy src (denied read) to public. Must fail.
out=$(SVN_USER=bob "$SVN_BIN" cp "$URL/src" "$URL/bob-stolen" \
        --author bob --log "steal attempt" 2>&1 || true)
tlib_check "bob can't copy denied subtree" "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# Super-user can (but a future phase might add an explicit opt-in).
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" cp "$URL/src" "$URL/src-mirror" \
        --author super --log "super copies" 2>&1)
tlib_check "super-user copies denied subtree" "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

tlib_stop_server
rm -rf "$REPO"

tlib_summary "test_acl_write"