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

# End-to-end test for aether-svnserver.
#
# Seeds a repo, starts the server, curls each endpoint, asserts responses,
# kills the server. Treats any mismatch as failure.
#
# Builds the seed + server binaries in /tmp; expects to be run from the
# project root. Requires curl, python3 (for JSON field extraction).
set -e
cd "$(dirname "$0")/../.."

AE=/home/paul/scm/aether/build/ae
PORT="${PORT:-9300}"
REPO=/tmp/svnae_test_server_repo
SERVER_BIN=/tmp/svnae_test_server
SEED_BIN=/tmp/svnae_test_seed

trap 'pkill -f "${SERVER_BIN} demo ${REPO} ${PORT}" 2>/dev/null || true' EXIT

echo "[*] Building svnserver + seeder..."
./regen.sh >/dev/null
"$AE" build ae/svnserver/main.ae -o "$SERVER_BIN" >/dev/null 2>&1
"$AE" build ae/svnserver/seed.ae -o "$SEED_BIN"  >/dev/null 2>&1

echo "[*] Seeding repo at $REPO ..."
rm -rf "$REPO"
"$SEED_BIN" "$REPO" >/dev/null

echo "[*] Launching server on :$PORT ..."
"$SERVER_BIN" demo "$REPO" "$PORT" >/tmp/svnae_server_out.log 2>&1 &
SRV=$!
sleep 1.5

FAILS=0
check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        echo "  ok   $label"
    else
        echo "  FAIL $label"
        echo "       expected: $expected"
        echo "       got     : $actual"
        FAILS=$((FAILS + 1))
    fi
}

# --- info ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/info")
head=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["head"])')
check "info head==3"        "3"    "$head"

name=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["name"])')
check "info name==demo"     "demo" "$name"

# --- log ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/log")
count=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
check "log count==4"        "4"    "$count"

alice1=$(echo "$r" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["entries"][1]["author"])')
check "log[1] author"       "alice" "$alice1"

bob2=$(echo "$r" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["entries"][2]["msg"])')
check "log[2] msg"          "answer to everything" "$bob2"

# --- rev/2/info ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/2/info")
author2=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["author"])')
check "rev/2 author"        "bob" "$author2"

# --- cat ---
c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/1/cat/README")
check "cat r1 README"       "Hello" "$c"

c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/2/cat/src/main.c")
check "cat r2 src/main.c"   "int main() { return 42; }" "$c"

c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/1/cat/src/main.c")
check "cat r1 src/main.c"   "int main() { return 0; }" "$c"

# --- list ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/1/list")
n=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
check "list r1 count"       "3" "$n"

first_name=$(echo "$r" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["entries"][0]["name"])')
check "list r1 [0]"         "LICENSE" "$first_name"

r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/3/list")
n=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
check "list r3 count"       "2" "$n"

r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/2/list/src")
n=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
check "list r2 src count"   "1" "$n"

# --- 404s ---
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/3/cat/LICENSE")
check "r3 cat LICENSE"      "404" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/nosuch/info")
check "unknown repo"        "404" "$code"

# --- POST /commit (read/write loop) ---
# Seeded repo is at head=3. Commit rev 4 adding a new file and
# deleting src/main.c.

COMMIT_JSON=$(python3 -c '
import base64, json
content = b"// added by HTTP client\n"
body = {
    "base_rev": 3,
    "author":   "http-client",
    "log":      "via POST /commit",
    "edits": [
        {"op":"add-file", "path":"CHANGELOG", "content": base64.b64encode(content).decode()},
        {"op":"delete",   "path":"src/main.c"},
    ],
}
print(json.dumps(body))
')

r=$(curl -sf -X POST \
    -H 'Content-Type: application/json' \
    -d "$COMMIT_JSON" \
    "http://127.0.0.1:$PORT/repos/demo/commit")
new_rev=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["rev"])')
check "commit returned rev" "4" "$new_rev"

# Head should now be 4.
head=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/info" | python3 -c 'import sys,json; print(json.load(sys.stdin)["head"])')
check "head after commit"   "4" "$head"

# CHANGELOG should have the new bytes.
c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/CHANGELOG")
check "cat r4 CHANGELOG"    "// added by HTTP client" "$c"

# src/main.c should be gone from rev 4.
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/src/main.c")
check "r4 main.c gone"      "404" "$code"

# But rev 3 should still have it (immutability).
c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/3/cat/src/main.c")
check "r3 main.c preserved" "int main() { return 42; }" "$c"

# Log should now have 5 entries.
count=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/log" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
check "log count after commit" "5" "$count"

# Malformed JSON -> 400.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d 'not valid json' "http://127.0.0.1:$PORT/repos/demo/commit")
check "malformed commit 400" "400" "$code"

# Commit against a missing repo -> 404.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d "$COMMIT_JSON" "http://127.0.0.1:$PORT/repos/nosuch/commit")
check "commit missing repo" "404" "$code"

kill "$SRV" 2>/dev/null || true
wait "$SRV" 2>/dev/null || true
rm -rf "$REPO"

if [ "$FAILS" -gt 0 ]; then
    echo ""
    echo "FAIL: $FAILS case(s) failed"
    exit 1
fi
echo ""
echo "test_server: OK"
