#!/bin/bash
# Full build: regenerate ae/**/_generated.c then invoke `ae build` for
# each binary declared in aether.toml. Drop-in replacement for running
# `ae build <target>` directly — gets the prerequisite codegen right.
#
# Usage:
#   ./build.sh               # build every [[bin]]
#   ./build.sh svn           # build one named binary (matches [[bin]].name)
#
# Toolchain comes from .aether_binaries/build/{ae,aetherc}, populated
# by ./sync-aether-deps.sh from $AETHER_HOME. Run that script if the
# directory is missing or stale.

set -e
cd "$(dirname "$0")"

AE="$(pwd)/.aether_binaries/build/ae"
if [ ! -x "$AE" ]; then
    cat >&2 <<'EOF'
build.sh: .aether_binaries/build/ae not found.

The Aether toolchain snapshot is missing. Populate it with:

    AETHER_HOME=/path/to/aether/checkout ./sync-aether-deps.sh

(See sync-aether-deps.sh for the source-tree shape it expects.)
EOF
    exit 2
fi

./regen.sh

target="${1:-}"
# Extract every [[bin]]'s name + path from aether.toml.
#   [[bin]]
#   name = "svn"
#   path = "ae/svn/main.ae"
AE="$AE" python3 - "$target" <<'PY'
import re, subprocess, sys, os
target = sys.argv[1] if len(sys.argv) > 1 else ""
with open("aether.toml") as f: toml = f.read()
bins = re.findall(r'\[\[bin\]\]\s*\nname\s*=\s*"([^"]+)"\s*\npath\s*=\s*"([^"]+)"', toml)
ae = os.environ["AE"]
for name, path in bins:
    if target and name != target: continue
    out = f"/tmp/{name}"
    print(f"[build] {name:30s} {path}")
    r = subprocess.run([ae, "build", path, "-o", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout); sys.stderr.write(r.stderr)
        sys.exit(r.returncode)
PY
