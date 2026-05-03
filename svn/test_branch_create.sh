#!/bin/bash

# Copyright 2026 Paul Hammant (portions).
# Apache License, Version 2.0 — see LICENSE.

# Phase 8.2a: svn branch create + spec storage.
#
# Covers:
#   (A) non-super create → 403.
#   (B) super create with includes succeeds; head file + spec on disk.
#   (C) filtered tree: include README only → feat tree has only README.
#   (D) include src/** picks up the whole src subtree (dir + descendants).
#   (E) missing --include → 2 (usage error).
#   (F) duplicate branch name → fails.
#   (G) bad base → fails.
#   (H) svn branch list shows main plus new branches with their globs.
#   (I) /info endpoint exposes the specs map.
#
# 8.2a does NOT yet enforce the spec on commits — that's 8.2b. So we
# only test the creation + listing path here.

source "$(dirname "$0")/../tests/lib.sh"

tlib_use_fixture test_branch_create

TOKEN="b82a-token"
URL="http://127.0.0.1:$PORT/demo"

tlib_seed "$REPO"
tlib_start_server "$PORT" "$REPO" demo --superuser-token "$TOKEN"

# --- (A) non-super create rejected ---
out=$("$SVN_BIN" branch create foo "$URL" --from main --include README 2>&1 || true)
tlib_check "non-super refused"  "0" "$(echo "$out" | grep -c 'Created branch' || true)"

# --- (B, C) super-user creates readme-only branch; spec + head on disk ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create readme-only "$URL" \
        --from main --include README 2>&1)
tlib_check "readme-only created"      "1" "$(echo "$out" | grep -c 'Created branch' || true)"
tlib_check "readme-only/head exists"  "1" "$(test -f "$REPO/branches/readme-only/head" && echo 1 || echo 0)"
tlib_check "readme-only/spec body"    "README" "$(cat "$REPO/branches/readme-only/spec")"

# Tree at that rev contains only README.
rev=$(grep -oE 'r[0-9]+' <<< "$out" | grep -oE '[0-9]+')
list=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/$rev/list")
tlib_check "readme-only tree has README" "1" "$(echo "$list" | grep -c '"name":"README"' || true)"
tlib_check "readme-only tree no src"     "0" "$(echo "$list" | grep -c '"name":"src"' || true)"

# --- (D) include src/** + README → feat has both ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create feat "$URL" \
        --from main --include 'src/**' --include README 2>&1)
tlib_check "feat created"  "1" "$(echo "$out" | grep -c 'Created branch' || true)"
rev=$(grep -oE 'r[0-9]+' <<< "$out" | grep -oE '[0-9]+')
list=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/$rev/list")
tlib_check "feat tree has README" "1" "$(echo "$list" | grep -c '"name":"README"' || true)"
tlib_check "feat tree has src"    "1" "$(echo "$list" | grep -c '"name":"src"' || true)"
list_src=$(curl -s "http://127.0.0.1:$PORT/repos/demo/rev/$rev/list/src")
tlib_check "feat/src has main.c"  "1" "$(echo "$list_src" | grep -c '"name":"main.c"' || true)"

# --- (E) no --include → 2 (usage) ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create bad "$URL" --from main 2>&1 || true)
tlib_check "no-include rejected"  "1" "$(echo "$out" | grep -c 'at least one --include' || true)"

# --- (F) duplicate branch name ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create feat "$URL" \
        --from main --include README 2>&1 || true)
tlib_check "duplicate refused"  "0" "$(echo "$out" | grep -c 'Created branch' || true)"

# --- (G) unknown base ---
out=$(SVN_SUPERUSER_TOKEN="$TOKEN" "$SVN_BIN" branch create x "$URL" \
        --from no-such --include README 2>&1 || true)
tlib_check "bad base refused"   "0" "$(echo "$out" | grep -c 'Created branch' || true)"

# --- (H) svn branch list shows everything with globs ---
out=$("$SVN_BIN" branch list "$URL")
tlib_check "list has main"          "1" "$(echo "$out" | grep -c '^main$' || true)"
tlib_check "list has readme-only"   "1" "$(echo "$out" | grep -c '^readme-only' || true)"
tlib_check "list has feat"          "1" "$(echo "$out" | grep -c '^feat' || true)"
tlib_check "list shows README glob" "1" "$(echo "$out" | grep -c '^readme-only.*README' || true)"
tlib_check "list shows src/** glob" "1" "$(echo "$out" | grep -c 'src/\*\*' || true)"

# --- (I) /info specs map ---
info=$(curl -s "http://127.0.0.1:$PORT/repos/demo/info")
tlib_check "info has specs"         "1" "$(echo "$info" | grep -c '"specs":{' || true)"
tlib_check "info specs readme-only" "1" "$(echo "$info" | grep -c '"readme-only":\["README"\]' || true)"

tlib_summary "test_branch_create"