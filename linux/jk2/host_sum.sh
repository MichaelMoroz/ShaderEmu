#!/usr/bin/env bash
# The sums of the plan's step 0 and 1 (docs/games/jk2.md, "Is it the same game"): the build computer's program
# (JK2_HOST=1 build.sh: no window, no picture, sound mixed into nothing) plays the demo's level for FRAMES frames of
# 50 ms of the game's time from seed 7, with no input, and prints a jk2sum: line every EVERY frames of the game. The
# machine's program, run the same way, must print the same lines.
#   wsl -- bash linux/jk2/host_sum.sh [FRAMES [EVERY]]      (default 4000 400; the demo by linux/jk2/data.py)
#   JK2_WORK=DIR    the host build's folder (~/jk2-host)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${JK2_WORK:-$HOME/jk2-host}
FRAMES=${1:-4000}
EVERY=${2:-400}
JK2_HOST=1 JK2_WORK=$WORK bash "$HERE/build.sh" > "$WORK.build.log" 2>&1 || { tail -20 "$WORK.build.log"; exit 1; }
# the demo's pak in DATA/base, from the cache (data.py: the licence is accepted with --accept-eula)
DATA=$WORK/data
if [ ! -e "$DATA/base/assets0.pk3" ]; then
    echo "no $DATA/base/assets0.pk3: python3 linux/jk2/data.py host $DATA --accept-eula"; exit 1
fi
# a home of nothing every time: the settings a run writes at its end (openjo_sp.cfg) and its autosave change the next
rm -rf "$WORK/sum-home" && mkdir -p "$WORK/sum-home"
cd "$WORK"
JK2_FRAMES=$FRAMES JK2_SUM=$EVERY JK2_SEED=7 ./jk2 +set fs_basepath "$DATA" +set fs_homepath "$WORK/sum-home" +set com_demo 1 \
    +set fixedtime 50 +set com_maxfps 0 +map demo 2>&1 | grep -E 'jk2sum:' | sed 's/^[0-9-]* [0-9:]* //'
