#!/usr/bin/env bash
# Builds the desktop's own programs (editor, file manager, paint, viewer, settings, monitor,
# and nxkey, which presses keys for a test) into build/images/linux/root, and lists them for
# the Start menu. After linux/nanox/build.sh.
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
for p in nxedit nxfiles nxpaint nxview nxsettings nxmon nxweb nxkey; do
    rv32-cc -O2 -Wall -Wno-unused-function -I"$MW/src/include" -I"$HERE/../userland" "$HERE/$p.c" "$MW/src/lib/libnano-X.a" -o "$WORK/src/apps/$p"
    riscv32-linux-strip -o "$OUT/bin/$p" "$WORK/src/apps/$p"
done
# the Start menu lists what the nxapps.* files name (linux/nanox/nxbar.c)
printf '%s\n' "Utilities/Editor=nxedit" "Utilities/Files=nxfiles" "Utilities/Paint=nxpaint" "Other/Web=nxweb" "Utilities/Monitor=nxmon" "Utilities/Settings=nxsettings" > "$OUT/share/nxapps.20-apps"
# the browser's pages: its home lists the sites of web/sites.txt (the VRChat world can ask for
# no others, so its builder reads the same file)
# (all in /usr/share: the image builder adds files to folders the ROM has, and makes none)
for f in "$HERE"/web/*.html "$HERE"/web/*.css; do cp "$f" "$OUT/share/web-$(basename "$f")"; done
python3 "$HERE/web/home.py" "$HERE/web/sites.txt" > "$OUT/share/web-index.html"
ls -l "$OUT/bin"/nxedit "$OUT/bin"/nxfiles "$OUT/bin"/nxpaint "$OUT/bin"/nxview "$OUT/bin"/nxsettings "$OUT/bin"/nxmon
