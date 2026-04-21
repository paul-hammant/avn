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
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9550}"
SERVER_BIN=/tmp/svnae_test_aclw_server
SEED_BIN=/tmp/svnae_test_aclw_seed
SVN_BIN=/tmp/svnae_test_aclw_svn

TOKEN="test-super-token-7-2"

trap 'pkill -f "${SERVER_BIN} demo" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

REPO=/tmp/svnae_test_aclw_repo
rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" --superuser-token "$TOKEN" >/tmp/svnae_test_aclw_srv.log 2>&1 &
SRV=$!
sleep 1.2

URL="http://127.0.0.1:$PORT/demo"

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# --- (A) Super-user sets ACL: alice:rw, chuck:r, everyone else denied.
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" acl set "$URL" src \
    "+alice:rw" "+chuck:r" "-*" >/dev/null

# --- (B) alice can commit in src. Uses the URL-flag commit path
#     (no WC needed) to keep the test tight.
out=$(SVN_USER=alice "$SVN_BIN" commit "$URL" \
        --author alice --log "alice adds file" \
        --add-file 'src/alice.txt=hello' 2>&1)
check "alice commits in src"  "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (C) bob (denied) cannot commit to src. Server returns 403.
out=$(SVN_USER=bob "$SVN_BIN" commit "$URL" \
        --author bob --log "bob tries" \
        --add-file 'src/bob.txt=nope' 2>&1 || true)
check "bob refused at src"    "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (D) chuck is read-only: his commit is refused.
out=$(SVN_USER=chuck "$SVN_BIN" commit "$URL" \
        --author chuck --log "chuck tries" \
        --add-file 'src/chuck.txt=readonly' 2>&1 || true)
check "chuck (r-only) refused" "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# ... but chuck can still READ src (confirms rw vs r distinction).
cat_chuck=$(SVN_USER=chuck "$SVN_BIN" cat "$URL" src/alice.txt)
check "chuck can read alice's file" "hello" "$cat_chuck"

# --- (E) anonymous (no user header) is refused on write.
out=$("$SVN_BIN" commit "$URL" \
        --author anon --log "anon tries" \
        --add-file 'src/anon.txt=noname' 2>&1 || true)
check "anon refused at src"   "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# Anon can commit OUTSIDE src (root has no ACL restricting it).
out=$("$SVN_BIN" commit "$URL" \
        --author anon --log "anon outside src" \
        --add-file 'public.txt=ok' 2>&1)
check "anon commits at root"  "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (F) self-elevation refused: bob tries to set +bob:rw on /src.
#     Server must refuse because bob has no write on /src.
out=$(SVN_USER=bob "$SVN_BIN" acl set "$URL" src "+bob:rw" 2>&1 || true)
check "bob can't self-elevate"  "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- (G) super-user bypasses everything.
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" commit "$URL" \
        --author super --log "super in src" \
        --add-file 'src/super.txt=ok' 2>&1)
check "super-user commits in src" "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

# --- Copy guard: bob tries to copy src (denied read) to public. Must fail.
out=$(SVN_USER=bob "$SVN_BIN" cp "$URL/src" "$URL/bob-stolen" \
        --author bob --log "steal attempt" 2>&1 || true)
check "bob can't copy denied subtree" "0" "$(echo "$out" | grep -c 'Committed revision' || true)"

# Super-user can (but a future phase might add an explicit opt-in).
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" cp "$URL/src" "$URL/src-mirror" \
        --author super --log "super copies" 2>&1)
check "super-user copies denied subtree" "1" "$(echo "$out" | grep -c 'Committed revision' || true)"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_acl_write: OK"
