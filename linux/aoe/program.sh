#!/bin/bash
# The program, flat, into a folder of its own (linux/aoe/data.py asks for it when an image wants the game):
#   program.sh OUT [PRIVATE_HOME]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$1
if [ -n "${2:-}" ]; then
    export HOME=$2 PYTHONUSERBASE=/home/micha/.local
fi
cd "$REPO"
AOE_OUT="$OUT" bash linux/nommu/build.sh aoe/build.sh
[ -f "$HOME/nm/logs/aoe-build.sh.log" ] && tail -2 "$HOME/nm/logs/aoe-build.sh.log" | cut -c1-160
[ -f "$OUT/usr/bin/aoe.bin" ] || { echo "aoe: no program in $OUT"; exit 1; }
python3 linux/nommu/flat.py "$OUT" | tail -1
