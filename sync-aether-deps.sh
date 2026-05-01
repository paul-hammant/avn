#!/bin/bash
# Snapshot the Aether toolchain + stdlib + contribs we depend on into
# .aether_binaries/. Every build path in this repo points at the local
# snapshot, not at the upstream checkout — re-run this script when you
# bump Aether.
#
# Required: AETHER_HOME must point at a built aether checkout (a
# directory containing build/aetherc + std/ + runtime/ + contrib/).
# We don't default to ~/scm/aether — explicit is better than guessing,
# and a missing AETHER_HOME should fail loudly rather than silently
# fall back to whatever happens to be on disk.
#
# Layout under .aether_binaries/ matches AETHER_HOME's layout so the
# `ae` and `aetherc` binaries find their sibling runtime/ and std/
# dirs exactly the way they do upstream:
#
#   .aether_binaries/
#     build/{ae,aetherc,libaether.a}
#     runtime/...               -- C headers the codegen pulls in
#     std/...                   -- C headers + Aether module.ae files
#     contrib/sqlite/           -- module.ae + aether_sqlite.c
#     STAMP                     -- source path + git rev for staleness checks
#
# We copy rather than symlink: the snapshot stays valid even if the
# upstream checkout moves, and a `git diff` style review of what
# actually changed becomes possible.

set -euo pipefail

if [ -z "${AETHER_HOME:-}" ]; then
    cat >&2 <<'EOF'
sync-aether-deps.sh: AETHER_HOME is not set.

Set it to point at a built Aether checkout, e.g.:

    AETHER_HOME=/home/paul/scm/aether ./sync-aether-deps.sh

The directory must contain build/aetherc + build/ae + std/ + runtime/
+ contrib/.
EOF
    exit 1
fi

if [ ! -d "$AETHER_HOME" ]; then
    echo "sync-aether-deps.sh: AETHER_HOME=$AETHER_HOME is not a directory" >&2
    exit 1
fi

required=(
    "build/aetherc"
    "build/ae"
    "build/libaether.a"
    "runtime"
    "std"
    "contrib/sqlite/module.ae"
    "contrib/sqlite/aether_sqlite.c"
)
for path in "${required[@]}"; do
    if [ ! -e "$AETHER_HOME/$path" ]; then
        echo "sync-aether-deps.sh: missing $AETHER_HOME/$path" >&2
        echo "  (did you run 'make' in the aether checkout?)" >&2
        exit 1
    fi
done

cd "$(dirname "$0")"
DEST=".aether_binaries"

echo "[sync] source:      $AETHER_HOME"
echo "[sync] destination: $(pwd)/$DEST"

# Wipe and recreate so removed files upstream get removed locally too.
rm -rf "$DEST"
mkdir -p "$DEST/build" "$DEST/contrib/sqlite"

# Binaries + libaether.a
cp "$AETHER_HOME/build/aetherc"     "$DEST/build/aetherc"
cp "$AETHER_HOME/build/ae"          "$DEST/build/ae"
cp "$AETHER_HOME/build/libaether.a" "$DEST/build/libaether.a"

# Runtime headers — the C codegen #include's these from the generated .c.
# Copy the whole tree; it's small and sub-selecting is fragile.
cp -r "$AETHER_HOME/runtime" "$DEST/runtime"

# stdlib — both the C-side headers (.h) and the Aether-side modules
# (module.ae) live here. The compiler walks the whole tree to resolve
# `import std.foo` and -I flags pull in any headers shims #include.
cp -r "$AETHER_HOME/std" "$DEST/std"

# contrib.sqlite — the only contrib module the subversion port uses
# today. If we add more, list them explicitly here rather than copying
# all of contrib/ (some of those entries pull large dependency trees).
cp "$AETHER_HOME/contrib/sqlite/module.ae"        "$DEST/contrib/sqlite/module.ae"
cp "$AETHER_HOME/contrib/sqlite/aether_sqlite.c"  "$DEST/contrib/sqlite/aether_sqlite.c"
# aether_sqlite.c does `#include "../../std/string/aether_string.h"`,
# which resolves correctly under .aether_binaries/contrib/sqlite/
# because std/ is a sibling of contrib/ — same layout as upstream.

# Stamp the snapshot so we can detect drift.
src_rev="(non-git)"
if git -C "$AETHER_HOME" rev-parse HEAD >/dev/null 2>&1; then
    src_rev="$(git -C "$AETHER_HOME" rev-parse HEAD)"
    src_branch="$(git -C "$AETHER_HOME" rev-parse --abbrev-ref HEAD 2>/dev/null || echo "?")"
    src_dirty=""
    if ! git -C "$AETHER_HOME" diff --quiet 2>/dev/null; then
        src_dirty=" (dirty)"
    fi
    src_rev="$src_rev on $src_branch$src_dirty"
fi
{
    echo "AETHER_HOME=$AETHER_HOME"
    echo "rev=$src_rev"
    echo "synced=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$DEST/STAMP"

# Summary.
ae_version="$("$DEST/build/ae" version 2>/dev/null | head -1 || echo "?")"
echo "[sync] ae version:  $ae_version"
echo "[sync] source rev:  $src_rev"
echo "[sync] done."
