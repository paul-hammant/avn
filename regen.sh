#!/bin/bash
# Regenerate every ae/**/_generated.c from its paired .ae source.
#
# The `.ae` → `.c` compilation is done by `aetherc --emit=lib`. The
# generated C is NOT checked into git — it's built on demand. This
# script runs every per-directory Makefile.regen so a fresh clone
# becomes buildable with one command.
#
# Invoked by:
#   - ./build.sh            (human workflow; wraps `ae build` too)
#   - every ae/*/test_*.sh  (before any `ae build`)
#   - CI (at the top of the test job)
#
# Fast by default — make only rebuilds files whose .ae is newer than
# the .c. Pass --force to rebuild everything.

set -e
cd "$(dirname "$0")"

AETHERC="${AETHERC:-$HOME/scm/aether/build/aetherc}"
if [ ! -x "$AETHERC" ]; then
    echo "regen.sh: aetherc not found at $AETHERC — set AETHERC=/path/to/aetherc" >&2
    exit 2
fi

FORCE=""
if [ "${1:-}" = "--force" ]; then FORCE="-B"; fi

for mk in ae/*/Makefile.regen; do
    make -C "$(dirname "$mk")" -f Makefile.regen AETHERC="$AETHERC" $FORCE
done
