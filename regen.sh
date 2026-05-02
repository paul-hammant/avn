#!/bin/bash
# Regenerate every ae/**/_generated.c from its paired .ae source via
# `aetherc --emit=lib`. The generated C is .gitignored — built on
# demand. This script runs every per-directory Makefile.regen so a
# fresh clone becomes buildable.
#
# Layer 2 of the swan-principle cleanup will eventually retire this
# (and the Makefile.regen files it invokes) by moving codegen into
# aeb's `aether.program(b) { regen(...) }` setter. That's blocked on
# aeb supporting multi-binary directories — see
# /home/paul/scm/aetherBuild/asks/multi-build-files-per-dir.md.
#
# Usage:
#   ./regen.sh             # incremental — rebuild only changed
#   ./regen.sh --force     # full rebuild

set -e
cd "$(dirname "$0")"

AETHERC="${AETHERC:-$(command -v aetherc)}"
if [ ! -x "$AETHERC" ]; then
    echo "regen.sh: aetherc not found. Install Aether or set AETHERC=." >&2
    exit 2
fi

if [ "${1:-}" = "--force" ]; then
    find ae -name "*_generated.c" -delete
fi

export AETHERC
for mk in $(find ae -name "Makefile.regen" | sort); do
    dir="$(dirname "$mk")"
    (cd "$dir" && make -f "$(basename "$mk")") || exit $?
done
