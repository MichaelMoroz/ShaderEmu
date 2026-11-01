#!/usr/bin/env bash
# A private build home for this work, from ~/nm, its drivers pointing at it (once): bash experiments/rv_play/home.sh
set -euo pipefail
A=$HOME/agents/play
[ -d "$A/nm/work" ] && { echo "there already"; exit 0; }
mkdir -p "$A"
cp -a "$HOME/nm" "$A/nm"
ln -sfn "$(readlink -f "$HOME/shaderemu-linux")" "$A/shaderemu-linux"
grep -rlI "$HOME/nm" "$A/nm/work/bin" | xargs -r sed -i "s#$HOME/nm#$A/nm#g"
head -3 "$A/nm/work/bin/rv32-cc"
du -sh "$A"
