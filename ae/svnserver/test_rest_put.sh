#!/bin/bash
# Phase 7.4: REST PUT / DELETE on /repos/{r}/path/<path>.
#
# Covers:
#   (A) GET current content at HEAD.
#   (B) PUT with correct Svn-Based-On updates the file (new rev, new sha).
#   (C) PUT with stale Svn-Based-On → 409 + X-Svnae-Current-Hash.
#   (D) PUT new file (no header) → 201.
#   (E) PUT existing file without Svn-Based-On → 409 (create-if-absent
#       semantics would overwrite, which is not what we want).
#   (F) DELETE with correct sha → 200 + path gone.
#   (G) DELETE with wrong sha → 409.
#   (H) DELETE on absent path → 404.
#   (I) Write-ACL honoured: bob (denied) gets 403 on PUT/DELETE.
#   (J) Author defaulted from X-Svnae-User when Svn-Author absent.
#   (K) curl -T scripted pattern works end-to-end.
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9580}"
REPO=/tmp/svnae_test_put_repo
SERVER_BIN=/tmp/svnae_test_put_server
SEED_BIN=/tmp/svnae_test_put_seed
SVN_BIN=/tmp/svnae_test_put_svn

TOKEN="test-put-token"
URL="http://127.0.0.1:$PORT/repos/demo"

trap 'pkill -f "${SERVER_BIN} demo" 2>/dev/null || true' EXIT

echo "[*] Build..."
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"   >/dev/null 2>&1
"$AE" build ae/svn/main.ae       -o "$SVN_BIN"    >/dev/null 2>&1

rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null
"$SERVER_BIN" demo "$REPO" "$PORT" --superuser-token "$TOKEN" >/tmp/svnae_test_put_srv.log 2>&1 &
SRV=$!
sleep 1.2

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then echo "  ok   $label"
    else echo "  FAIL $label"; echo "    expected: $expected"; echo "    got:      $actual"; FAILS=$((FAILS+1))
    fi
}

# Helper: pull X-Svnae-Node-Hash out of a response headers dump.
node_hash_of() {
    curl -sD - -o /dev/null "$1" \
        | awk 'BEGIN{IGNORECASE=1} /^X-Svnae-Node-Hash:/{gsub(/\r/,""); print $2}'
}

# --- (A) GET returns current content. ---
body=$(curl -s "$URL/path/src/main.c")
check "GET body"    "int main() { return 42; }" "$body"

# --- (B) PUT with correct Svn-Based-On. ---
sha=$(node_hash_of "$URL/path/src/main.c")
echo -n "updated content" > /tmp/put_body
code=$(curl -s -o /tmp/put_resp -w '%{http_code}' -X PUT \
         --data-binary @/tmp/put_body \
         -H "Svn-Based-On: $sha" \
         -H "Svn-Author: alice" \
         -H "Svn-Log: direct PUT" \
         "$URL/path/src/main.c")
check "PUT ok status"  "201" "$code"
check "PUT body has rev" "1" "$(grep -c '"rev":' /tmp/put_resp || true)"
# Content round-trip.
body=$(curl -s "$URL/path/src/main.c")
check "PUT content visible" "updated content" "$body"

# --- (C) PUT with stale Svn-Based-On → 409. ---
code=$(curl -s -o /tmp/put_resp -D /tmp/put_hdrs -w '%{http_code}' -X PUT \
         --data-binary @/tmp/put_body \
         -H "Svn-Based-On: 0000000000000000000000000000000000000000" \
         "$URL/path/src/main.c")
check "stale PUT 409"  "409" "$code"
check "stale PUT hdr"  "1"   "$(grep -ci 'X-Svnae-Current-Hash' /tmp/put_hdrs || true)"

# --- (D) PUT new file, no based-on header → 201. ---
echo -n "brand new" > /tmp/new_body
code=$(curl -s -o /tmp/put_resp -w '%{http_code}' -X PUT \
         --data-binary @/tmp/new_body \
         -H "Svn-Author: alice" \
         "$URL/path/newfile.txt")
check "PUT new 201"    "201" "$code"
body=$(curl -s "$URL/path/newfile.txt")
check "new file content" "brand new" "$body"

# --- (E) PUT existing file without based-on → 409. ---
code=$(curl -s -o /tmp/put_resp -w '%{http_code}' -X PUT \
         --data-binary @/tmp/put_body \
         "$URL/path/newfile.txt")
check "PUT exists without hdr 409" "409" "$code"

# --- (F) DELETE with correct sha → 200. ---
sha=$(node_hash_of "$URL/path/newfile.txt")
code=$(curl -s -o /tmp/put_resp -w '%{http_code}' -X DELETE \
         -H "Svn-Based-On: $sha" \
         -H "Svn-Author: alice" \
         "$URL/path/newfile.txt")
check "DELETE ok"      "200" "$code"
code=$(curl -s -o /dev/null -w '%{http_code}' "$URL/path/newfile.txt")
check "DELETE removed" "404" "$code"

# --- (G) DELETE with wrong sha → 409. ---
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE \
         -H "Svn-Based-On: deadbeefdeadbeefdeadbeefdeadbeefdeadbeef" \
         "$URL/path/src/main.c")
check "wrong-sha DELETE 409" "409" "$code"

# --- (H) DELETE on absent path → 404. ---
code=$(curl -s -o /dev/null -w '%{http_code}' -X DELETE \
         -H "Svn-Based-On: 0000000000000000000000000000000000000000" \
         "$URL/path/does/not/exist.txt")
check "DELETE absent 404" "404" "$code"

# --- (I) Write-ACL honoured. super-user sets ACL on src denying bob. ---
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" acl set \
    "http://127.0.0.1:$PORT/demo" src "+alice:rw" "-bob" "+*" >/dev/null

sha=$(curl -s -H "X-Svnae-Superuser: $TOKEN" -D - -o /dev/null \
        "$URL/path/src/main.c" | awk '/X-Svnae-Node-Hash/{gsub(/\r/,""); print $2}')
code=$(curl -s -o /dev/null -w '%{http_code}' -X PUT \
         --data-binary "hack" \
         -H "X-Svnae-User: bob" \
         -H "Svn-Based-On: $sha" \
         "$URL/path/src/main.c")
check "bob PUT denied 403"  "403" "$code"

# alice still can.
code=$(curl -s -o /tmp/put_resp -w '%{http_code}' -X PUT \
         --data-binary "alice update" \
         -H "X-Svnae-User: alice" \
         -H "Svn-Based-On: $sha" \
         "$URL/path/src/main.c")
check "alice PUT ok"   "201" "$code"

# --- (J) Author defaults from X-Svnae-User. ---
sha=$(curl -s -H "X-Svnae-Superuser: $TOKEN" -D - -o /dev/null \
        "$URL/path/src/main.c" | awk '/X-Svnae-Node-Hash/{gsub(/\r/,""); print $2}')
curl -s -o /dev/null -X PUT --data-binary "round2" \
    -H "X-Svnae-User: alice" \
    -H "Svn-Based-On: $sha" \
    "$URL/path/src/main.c"
# Verify last rev's author via /log
log=$(curl -s "$URL/log")
# Log entries are newest-last; last entry should be alice.
check "author = alice" "1" "$(echo "$log" | grep -c '"author":"alice"' || true)"

# --- (K) curl -T scripted pattern (the classic "upload a file" form). ---
echo -n "scripted" > /tmp/upload_me
sha=$(curl -s -H "X-Svnae-Superuser: $TOKEN" -D - -o /dev/null \
        "$URL/path/src/main.c" | awk '/X-Svnae-Node-Hash/{gsub(/\r/,""); print $2}')
code=$(curl -s -o /tmp/put_resp -w '%{http_code}' -T /tmp/upload_me \
         -H "X-Svnae-User: alice" \
         -H "Svn-Based-On: $sha" \
         "$URL/path/src/main.c")
check "curl -T 201"   "201" "$code"
body=$(curl -s -H "X-Svnae-User: alice" "$URL/path/src/main.c")
check "curl -T body"   "scripted" "$body"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO" /tmp/put_body /tmp/new_body /tmp/put_resp /tmp/put_hdrs /tmp/upload_me

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s)"
    exit 1
fi
echo ""
echo "test_rest_put: OK"
