#!/usr/bin/env bash
# Builds the desktop's own programs (editor, file manager, paint, viewer, settings, monitor)
# into build/images/linux/root, and lists them for the Start menu. After linux/nanox/build.sh.
#   wsl -- bash /mnt/c/.../linux/apps/build.sh
# The desktop's pictures come from tools/make_wallpaper.py (run on the host).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
OUT=$REPO/build/images/linux/root/usr
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
mkdir -p "$OUT/bin" "$OUT/share" "$WORK/src/apps"
for p in nxedit nxfiles nxpaint nxview nxsettings nxmon; do
    rv32-cc -O2 -Wall -Wno-unused-function -I"$MW/src/include" "$HERE/$p.c" "$MW/src/lib/libnano-X.a" -o "$WORK/src/apps/$p"
    riscv32-linux-strip -o "$OUT/bin/$p" "$WORK/src/apps/$p"
done
# the Start menu lists what the nxapps.* files name (linux/nanox/nxbar.c)
printf '%s\n' "Editor=nxedit" "Files=nxfiles" "Paint=nxpaint" "Monitor=nxmon" "Settings=nxsettings" > "$OUT/share/nxapps.20-apps"
ls -l "$OUT/bin"/nxedit "$OUT/bin"/nxfiles "$OUT/bin"/nxpaint "$OUT/bin"/nxview "$OUT/bin"/nxsettings "$OUT/bin"/nxmon
