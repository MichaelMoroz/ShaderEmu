#!/usr/bin/env bash
# The root with wl in /usr/bin (programs/wl/README.md), after build.sh. In WSL:
#   bash programs/wl/image.sh       -> build/wl/rvroot: linux/prebuilt/nommu's root and wl, for linux/nommu/image.py
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=$REPO/build/wl
case "${1:-rv}" in
rv)
    R=$OUT/rvroot
    rm -rf "$R"; cp -a "$REPO/linux/prebuilt/nommu/root" "$R"
    cp "$OUT/rv/wl" "$R/usr/bin/wl"
    echo "image.sh: $R; now (Windows): set HALFLIFE_DATA=0 & python linux/nommu/image.py nommu_wl build/wl/rvroot linux/prebuilt/nommu/Image"
    ;;
*) echo "image.sh [rv]"; exit 1 ;;
esac
