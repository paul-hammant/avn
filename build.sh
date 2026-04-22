#!/bin/bash
# Full build: regenerate ae/**/_generated.c then invoke `ae build` for
# each binary declared in aether.toml. Drop-in replacement for running
# `ae build <target>` directly — gets the prerequisite codegen right.
#
# Usage:
#   ./build.sh               # build every [[bin]]
#   ./build.sh svn           # build one named binary (matches [[bin]].name)
#   AETHERC=/path ./build.sh # override the aetherc location
#
# Env:
#   AE       -- ae CLI (default: ~/scm/aether/build/ae)
#   AETHERC  -- passed through to regen.sh (default: ~/scm/aether/build/aetherc)

set -e
cd "$(dirname "$0")"

AE="${AE:-$HOME/scm/aether/build/ae}"
if [ ! -x "$AE" ]; then
    echo "build.sh: ae not found at $AE — set AE=/path/to/ae" >&2
    exit 2
fi

./regen.sh

target="${1:-}"
# Extract every [[bin]]'s name + path from aether.toml.
#   [[bin]]
#   name = "svn"
#   path = "ae/svn/main.ae"
python3 - "$target" <<'PY'
import re, subprocess, sys, os
target = sys.argv[1] if len(sys.argv) > 1 else ""
with open("aether.toml") as f: toml = f.read()
bins = re.findall(r'\[\[bin\]\]\s*\nname\s*=\s*"([^"]+)"\s*\npath\s*=\s*"([^"]+)"', toml)
ae = os.environ.get("AE", os.path.expanduser("~/scm/aether/build/ae"))
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
