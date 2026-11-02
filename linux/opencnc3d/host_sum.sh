#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The build computer's sums of the plan's step 1 (docs/games/opencnc3d.md, "Is it the same game"): the brain built
# natively with no picture (OPENCNC3D_HOST=1 build.sh) plays a mission of the cartridge for TICKS ticks from the
# game's own start and prints an ocnc: sum line every EVERY ticks; the machine's program, run the same way, must
# print the same lines. Checks the objects at the start against the mission's INI too.
#   wsl -- bash linux/opencnc3d/host_sum.sh CACHE [SCEN [TICKS [EVERY]]]     (SCG01EA 3000 300)
#   CACHE: bake.py's and data.py's cache (its dos/ and missions/)
#   OPENCNC3D_WORK=DIR    the host build's folder (~/ocnc-host)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
CACHE=$(cd "$1" && pwd)
SCEN=${2:-SCG01EA}
TICKS=${3:-3000}
EVERY=${4:-300}
WORK=${OPENCNC3D_WORK:-$HOME/ocnc-host}
OPENCNC3D_HOST=1 OPENCNC3D_WORK=$WORK bash "$HERE/build.sh" > "$WORK.build.log" 2>&1 || { tail -20 "$WORK.build.log"; exit 1; }
cd "$WORK"
./opencnc3d-brain "$CACHE/dos/" "$CACHE/missions/" "$SCEN" "$TICKS" "$EVERY" > "$WORK/sum-$SCEN.log" 2>&1
python3 "$HERE/check_objects.py" "$CACHE/missions/$SCEN.INI" "$WORK/sum-$SCEN.log"
grep -E '^ocnc: (start|at) ' "$WORK/sum-$SCEN.log"
