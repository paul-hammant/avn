#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Phase 8.2b: branch-spec enforcement on commits.
#
# Covers:
#   (A) PUT to path outside readme-only's spec → 403.
#   (B) PUT to README on readme-only succeeds.
#   (C) PUT to src/foo.c on feat (spec src/**) succeeds.
#   (D) PUT to README on feat (spec src/** + README) succeeds.
#   (E) /commit bulk with mixed-branch-violating paths → 403 (atomic).
#   (F) server-side cp whose from_path is outside branch spec → 403.
#   (G) server-side cp whose to_path is outside branch spec → 403.
#   (H) main branch (no spec) accepts everything (legacy behaviour).
#   (I) super-user bypass: super can PUT outside any branch spec.

source "$(dirname "$0")/../tests/lib.sh"

tlib_use_fixture test_branch_enforce

TOKEN="b82b-token"
URL="http://127.0.0.1:$PORT/demo"

tlib_seed "$REPO"
tlib_start_server "$PORT" "$REPO" demo --superuser-token "$TOKEN"

# Create two branches:
#   readme-only: README only
#   feat:        src/** + README
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create readme-only "$URL" \
    --from main --include README >/dev/null
SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create feat "$URL" \
    --from main --include 'src/**' --include README >/dev/null

# Helper: PUT. If the path already exists, pass its current sha via
# Svn-Based-On; otherwise omit the header (create-new semantics).
put_branch() {
    local branch="$1" path="$2" body="$3"
    local cur; cur=$(curl -s -D /tmp/ph -o /dev/null "http://127.0.0.1:$PORT/repos/demo/path/$path" ; \
                     grep -i '^X-Svnae-Node-Hash:' /tmp/ph | awk '{print $2}' | tr -d '\r')
    local based_on=()
    if [ -n "$cur" ]; then based_on=(-H "Svn-Based-On: $cur"); fi
    curl -s -o /tmp/p.out -w '%{http_code}' -X PUT \
        -H "Svn-Branch: $branch" \
        -H "Svn-Author: alice" \
        "${based_on[@]}" \
        -H "Content-Type: application/octet-stream" \
        --data-binary "$body" \
        "http://127.0.0.1:$PORT/repos/demo/path/$path"
}

put_branch_super() {
    local branch="$1" path="$2" body="$3"
    local cur; cur=$(curl -s -D /tmp/ph -o /dev/null "http://127.0.0.1:$PORT/repos/demo/path/$path" ; \
                     grep -i '^X-Svnae-Node-Hash:' /tmp/ph | awk '{print $2}' | tr -d '\r')
    local based_on=()
    if [ -n "$cur" ]; then based_on=(-H "Svn-Based-On: $cur"); fi
    curl -s -o /tmp/p.out -w '%{http_code}' -X PUT \
        -H "Svn-Branch: $branch" \
        -H "X-Svnae-Superuser: $TOKEN" \
        -H "Svn-Author: super" \
        "${based_on[@]}" \
        -H "Content-Type: application/octet-stream" \
        --data-binary "$body" \
        "http://127.0.0.1:$PORT/repos/demo/path/$path"
}

# --- (A) PUT src/main.c on readme-only → 403 (spec only allows README) ---
code=$(put_branch readme-only "src/main.c" "x")
tlib_check "readme-only rejects src/main.c"   "403"  "$code"

# --- (B) PUT README on readme-only → 201 ---
code=$(put_branch readme-only "README" "hello")
tlib_check "readme-only accepts README"       "201"  "$code"

# --- (C) PUT src/foo.c on feat → 201 ---
code=$(put_branch feat "src/foo.c" "bar")
tlib_check "feat accepts src/foo.c"           "201"  "$code"

# --- (D) PUT README on feat → 201 ---
code=$(put_branch feat "README" "readme2")
tlib_check "feat accepts README"              "201"  "$code"

# --- (E) /commit bulk with mixed bad path → 403 atomic ---
base=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info" | grep -oE '"head":[0-9]+' | grep -oE '[0-9]+')
b64_r=$(printf 'bar' | base64 | tr -d '\n')
code=$(curl -s -o /tmp/p.out -w '%{http_code}' -X POST \
    -H "Content-Type: application/json" \
    -d "{\"base_rev\":$base,\"branch\":\"readme-only\",\"author\":\"alice\",\"log\":\"m\",\"edits\":[{\"op\":\"add-file\",\"path\":\"README\",\"content\":\"$b64_r\"},{\"op\":\"add-file\",\"path\":\"src/x.c\",\"content\":\"$b64_r\"}]}" \
    "http://127.0.0.1:$PORT/repos/demo/commit")
tlib_check "bulk commit rejects mixed bad"    "403"  "$code"
# Verify atomic: head rev did not advance.
head2=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info" | grep -oE '"head":[0-9]+' | grep -oE '[0-9]+')
tlib_check "bulk commit atomic (no advance)"  "$base"  "$head2"

# --- (F) server-side cp whose from_path is outside feat spec → 403 ---
# Seed a file outside both specs (as super, against main which has no spec).
code=$(put_branch_super main "docs/note.txt" "note-body")
tlib_check "super PUT main docs/note.txt"     "201"  "$code"
base=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info" | grep -oE '"head":[0-9]+' | grep -oE '[0-9]+')
code=$(curl -s -o /tmp/p.out -w '%{http_code}' -X POST \
    -H "Content-Type: application/json" \
    -H "Svn-Branch: feat" \
    -d "{\"base_rev\":$base,\"from_path\":\"docs/note.txt\",\"to_path\":\"src/note.txt\",\"author\":\"alice\",\"log\":\"cp\"}" \
    "http://127.0.0.1:$PORT/repos/demo/copy")
tlib_check "cp from outside feat spec → 403"  "403"  "$code"

# --- (G) cp whose to_path escapes feat spec → 403 ---
code=$(curl -s -o /tmp/p.out -w '%{http_code}' -X POST \
    -H "Content-Type: application/json" \
    -H "Svn-Branch: feat" \
    -d "{\"base_rev\":$base,\"from_path\":\"src\",\"to_path\":\"docs/src-copy\",\"author\":\"alice\",\"log\":\"cp\"}" \
    "http://127.0.0.1:$PORT/repos/demo/copy")
tlib_check "cp to outside feat spec → 403"    "403"  "$code"

# --- (H) main (no spec) accepts arbitrary paths from normal users ---
code=$(put_branch main "anywhere/else.txt" "ok")
tlib_check "main accepts arbitrary path"      "201"  "$code"

# --- (I) super bypasses readme-only's spec ---
code=$(put_branch_super readme-only "src/other.c" "x")
tlib_check "super PUT readme-only src/other.c" "201"  "$code"

tlib_summary "test_branch_enforce"