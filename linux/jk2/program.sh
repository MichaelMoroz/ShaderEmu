#!/bin/bash
# The program for the machine, flat, into a folder of its own (linux/jk2/data.py asks for it when an image wants the
# game, as linux/aoe/program.sh): written 2 November 2026 and NOT YET RUN (the guest's toolchain was another build's
# that day; docs/games/jk2.md, "What is left").
#   program.sh OUT [PRIVATE_HOME]
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$1
if [ -n "${2:-}" ]; then
    export HOME=$2
fi
cd "$REPO"
JK2_OUT="$OUT" bash linux/nommu/build.sh jk2/build.sh
[ -f "$HOME/nm/logs/jk2-build.sh.log" ] && tail -2 "$HOME/nm/logs/jk2-build.sh.log" | cut -c1-160
[ -f "$OUT/usr/bin/jk2.bin" ] || { echo "jk2: no program in $OUT"; exit 1; }
python3 linux/nommu/flat.py "$OUT" | tail -1
