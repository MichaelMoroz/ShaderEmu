#!/usr/bin/env bash
# Uplink's installer fetched (md5-checked) and unpacked by REWise into build/halflife/uplink (the game's
# folder, media, logo.bmp, readme.txt, eula.txt), with delta.lst and the ADPCM encoder repack.py runs: data.py's
# first half, in WSL. Each step once.
#   wsl -- bash linux/halflife/data.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
B=$REPO/build/halflife
INSTALLER=${HALFLIFE_INSTALLER:-https://archive.org/download/hluplink/hluplink.exe}
INSTALLER_MD5=498afa6af130a64ff5966774654b0f18
VALVE_SDK_COMMIT=b1b5cf5892918535619b2937bb927e46cb097ba1   # ValveSoftware/halflife: network/delta.lst
REWISE=$B/rewise
mkdir -p "$B"
if [ ! -f "$B/uplink/valve/pak0.PAK" ] || [ ! -f "$B/uplink/media/intro.avi" ] || [ ! -f "$B/uplink/eula.txt" ]; then
    if [ ! -x "$REWISE/build/rewise" ]; then
        [ -d "$REWISE/.git" ] || git clone -q https://codeberg.org/CYBERDEV/REWise.git "$REWISE"
        git -C "$REWISE" checkout -q c3d3b68
        cmake -S "$REWISE" -B "$REWISE/build" -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build "$REWISE/build" -j"$(nproc)" > /dev/null
    fi
    case "$INSTALLER" in http*) [ -f "$B/hluplink.exe" ] || curl -sSfL "$INSTALLER" -o "$B/hluplink.exe" ;;
                         *) cp "$INSTALLER" "$B/hluplink.exe" ;; esac
    [ "$(md5sum < "$B/hluplink.exe" | cut -d' ' -f1)" = $INSTALLER_MD5 ] || { echo "hluplink.exe is not Uplink's installer"; exit 1; }
    T=$(mktemp -d)
    "$REWISE/build/rewise" -s -t /tmp/ -x "$T" "$B/hluplink.exe"
    rm -rf "$B/uplink" && mkdir -p "$B/uplink"
    cp -r "$T/MAINDIR/valve" "$B/uplink/valve"
    # (the launcher's: its splash, the films, the menu's sounds, the readme and the order page)
    cp -r "$T/MAINDIR/media" "$T/MAINDIR/logo.bmp" "$T/MAINDIR/readme.txt" "$B/uplink/"
    # (and Sierra's licence the installer showed before it installed: the starter shows it, docs/system/install.md)
    cp "$T/TEMP/EULAFILE" "$B/uplink/eula.txt" && rm -rf "$T"
fi
[ -f "$B/uplink/delta.lst" ] || curl -sSfL "https://raw.githubusercontent.com/ValveSoftware/halflife/$VALVE_SDK_COMMIT/network/delta.lst" -o "$B/uplink/delta.lst"
[ "$B/adpcm_encode" -nt "$REPO/linux/classicube/sound/adpcm_encode.c" ] || gcc -O2 -o "$B/adpcm_encode" "$REPO/linux/classicube/sound/adpcm_encode.c"
echo "Uplink's data in build/halflife/uplink"
