#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# End-to-end test for aether-svnserver.
#
# Seeds a repo, starts the server, curls each endpoint, asserts responses,
# kills the server. Treats any mismatch as failure.
#
# Builds the seed + server binaries in /tmp; expects to be run from the
# project root. Requires curl, python3 (for JSON field extraction).

source "$(dirname "$0")/../tests/lib.sh"

# Server fixture spawned by aeb (.tests-server.ae). The fixture
# exports test_server_PORT and test_server_REPO into our env.
tlib_use_fixture test_server

# --- info ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/info")
head=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["head"])')
tlib_check "info head==3"        "3"    "$head"

name=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["name"])')
tlib_check "info name==demo"     "demo" "$name"

# --- log ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/log")
count=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
tlib_check "log count==4"        "4"    "$count"

alice1=$(echo "$r" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["entries"][1]["author"])')
tlib_check "log[1] author"       "alice" "$alice1"

bob2=$(echo "$r" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["entries"][2]["msg"])')
tlib_check "log[2] msg"          "answer to everything" "$bob2"

# --- rev/2/info ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/2/info")
author2=$(echo "$r" | python3 -c 'import sys,json; print(json.load(sys.stdin)["author"])')
tlib_check "rev/2 author"        "bob" "$author2"

# --- cat ---
c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/1/cat/README")
tlib_check "cat r1 README"       "Hello" "$c"

c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/2/cat/src/main.c")
tlib_check "cat r2 src/main.c"   "int main() { return 42; }" "$c"

c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/1/cat/src/main.c")
tlib_check "cat r1 src/main.c"   "int main() { return 0; }" "$c"

# --- list ---
r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/1/list")
n=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
tlib_check "list r1 count"       "3" "$n"

first_name=$(echo "$r" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["entries"][0]["name"])')
tlib_check "list r1 [0]"         "LICENSE" "$first_name"

r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/3/list")
n=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
tlib_check "list r3 count"       "2" "$n"

r=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/2/list/src")
n=$(echo "$r" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
tlib_check "list r2 src count"   "1" "$n"

# --- 404s ---
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/3/cat/LICENSE")
tlib_check "r3 cat LICENSE"      "404" "$code"

code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/nosuch/info")
tlib_check "unknown repo"        "404" "$code"

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
tlib_check "commit returned rev" "4" "$new_rev"

# Head should now be 4.
head=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/info" | python3 -c 'import sys,json; print(json.load(sys.stdin)["head"])')
tlib_check "head after commit"   "4" "$head"

# CHANGELOG should have the new bytes.
c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/CHANGELOG")
tlib_check "cat r4 CHANGELOG"    "// added by HTTP client" "$c"

# src/main.c should be gone from rev 4.
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/repos/demo/rev/4/cat/src/main.c")
tlib_check "r4 main.c gone"      "404" "$code"

# But rev 3 should still have it (immutability).
c=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/rev/3/cat/src/main.c")
tlib_check "r3 main.c preserved" "int main() { return 42; }" "$c"

# Log should now have 5 entries.
count=$(curl -sf "http://127.0.0.1:$PORT/repos/demo/log" | python3 -c 'import sys,json; print(len(json.load(sys.stdin)["entries"]))')
tlib_check "log count after commit" "5" "$count"

# Malformed JSON -> 400.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d 'not valid json' "http://127.0.0.1:$PORT/repos/demo/commit")
tlib_check "malformed commit 400" "400" "$code"

# Commit against a missing repo -> 404.
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
    -d "$COMMIT_JSON" "http://127.0.0.1:$PORT/repos/nosuch/commit")
tlib_check "commit missing repo" "404" "$code"

tlib_summary "test_server"
