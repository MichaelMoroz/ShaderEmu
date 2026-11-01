#!/usr/bin/env bash
# A second private home at another path, a clean build of the game in it, against the first: does the result hold outside its folder? home2.sh (from the worktree)
set -euo pipefail
A=$HOME/agents/play; B=$HOME/agents/play_second_home
chmod -R u+w "$B" 2>/dev/null || true
rm -rf "$B"; mkdir -p "$B"
rsync -a --exclude work/src/tdawn-build --exclude work/src/ralert-build --exclude 'work/src/quake*' --exclude 'work/src/halflife*' --exclude 'work/src/openttd*' "$A/nm" "$B/"
ln -s "$HOME/shaderemu-linux" "$B/shaderemu-linux"
grep -rlI "$A" "$B/nm/work/bin" | xargs -r sed -i "s#$A#$B#g"
export REALHOME=$HOME HOME=$B PYTHONUSERBASE=$HOME/.local NM=$B/nm W=$B/nm/work WORK=$B/nm/work
export PATH=$W/bin:$REALHOME/shaderemu-linux/toolchain/bin:$PATH
bash linux/nommu/build.sh tdawn/build.sh
"$REALHOME/shaderemu-linux/toolchain/bin/riscv32-linux-strip" -o "$B/game" "$W/src/tdawn-build/vanillatd"
"$REALHOME/shaderemu-linux/toolchain/bin/riscv32-linux-strip" -o "$A/game.stripped" "$A/nm/work/src/tdawn-build/vanillatd"
sha1sum "$B/game" "$A/game.stripped"
