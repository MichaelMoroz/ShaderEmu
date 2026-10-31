#!/usr/bin/env bash
# The title screen's game, opntitle.dat: a 64 x 64 map of title.cfg made by the native build of
# host_sum.sh, which loads in a tenth of upstream's (docs/games/openttd.md, "The start").
#   wsl -- bash /mnt/c/.../linux/openttd/make_title.sh   (after host_sum.sh; writes linux/openttd/opntitle.dat)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
NATIVE=${WORK:-$HOME/nm/work}/src/openttd-native
[ -x "$NATIVE/openttd" ] || { echo "run linux/openttd/host_sum.sh first"; exit 1; }
H=$(mktemp -d)
cp "$HERE/title.cfg" "$H/title.cfg"
(sleep 4; echo "save $H/title"; sleep 3; echo quit) |
    (cd "$NATIVE" && XDG_DATA_HOME=$H XDG_CONFIG_HOME=$H HOME=$H ./openttd -D -g -c "$H/title.cfg" > "$H/log" 2>&1) || true
[ -f "$H/title.sav" ] || { tail -20 "$H/log"; exit 1; }
cp "$H/title.sav" "$HERE/opntitle.dat"
ls -l "$HERE/opntitle.dat"; rm -rf "$H"
