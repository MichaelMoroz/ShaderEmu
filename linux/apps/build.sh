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
MEDIA=$WORK/src/libmedia/libmedia.a
[ -f "$MEDIA" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }
# (the workers' library, compiled once for the programs that link libmedia)
rv32-cc -O2 -w -I"$REPO/programs/mc" -c "$REPO/programs/mc/mcw.c" -o "$WORK/src/libmedia/mcw.o"
rv32-cc -O2 -w -I"$REPO/programs/mc" -c "$REPO/programs/mc/mcwx.c" -o "$WORK/src/libmedia/mcwx.o"
mkdir -p "$OUT/bin" "$OUT/share" "$WORK/src/apps"
# nxpersist packs with zlib (docs/system/persist.md): its source is fetched once
Z=$WORK/src/zlib-1.3.1
[ -f "$Z/deflate.c" ] || (cd "$WORK/src" && curl -fsSL -o zlib-1.3.1.tar.gz https://zlib.net/fossils/zlib-1.3.1.tar.gz && tar -xzf zlib-1.3.1.tar.gz)
ZLIB="-w -I$Z $Z/deflate.c $Z/trees.c $Z/inflate.c $Z/inftrees.c $Z/inffast.c $Z/adler32.c $Z/crc32.c $Z/zutil.c"
# the programs compile at once, a process each; the build fails if any of them does
build_one() {
    local p=$1 more=""
    [ $p = nxpersist ] && more=$ZLIB
    [ $p = nxcalc ] && more="-lm"
    # the monitor and Paint move pictures with the worker cores (ui_bulk.h) and the browser
    # makes its strips there: the library every program has for them
    [ $p = nxview ] || [ $p = nxmon ] || [ $p = nxpaint ] || [ $p = nxweb ] && more="-I$REPO/programs/mc $REPO/programs/mc/mcw.c"
    # pictures of every kind, decoded and encoded on the worker cores (linux/libmedia, docs/system/media.md)
    case $p in nxview|nxpaint|nxfiles|nxsettings|nxicon|nxweb)
        more="-I$REPO/linux/libmedia -I$REPO/programs/mc $MEDIA $WORK/src/libmedia/mcw.o $WORK/src/libmedia/mcwx.o -lm -Wl,--gc-sections" ;;
    esac
    rv32-cc -O2 -Wall -Wno-unused-function -I"$MW/src/include" -I"$HERE/../userland" "$HERE/$p.c" $more "$MW/src/lib/libnano-X.a" -o "$WORK/src/apps/$p"
    riscv32-linux-strip -o "$OUT/bin/$p" "$WORK/src/apps/$p"
}
pids=()
for p in nxedit nxfiles nxpaint nxview nxsettings nxmon nxweb nxkey nxfuzz nxicon nxask nxnet open nxpersist nxreg nxtetris nxmine nxpinball nxeyes nxclock nxterm nxcalc; do
    build_one $p & pids+=($!)
done
# archives unpacked on the worker cores (unarc.c: open --unpack runs it first, docs/system/open.md)
( rv32-cc -O2 -Wall -Wno-unused-function -I"$REPO/linux/libmedia" -I"$REPO/programs/mc" "$HERE/unarc.c" "$MEDIA" \
    "$WORK/src/libmedia/mcw.o" "$WORK/src/libmedia/mcwx.o" -lm -Wl,--gc-sections -o "$WORK/src/apps/unarc"
  riscv32-linux-strip -o "$OUT/bin/unarc" "$WORK/src/apps/unarc" ) & pids+=($!)
failed=0
for pid in "${pids[@]}"; do wait $pid || failed=1; done
[ $failed = 0 ] || { echo "a program did not build"; exit 1; }
# the Start menu lists what the nxapps.* files name (linux/nanox/nxbar.c)
printf '%s\n' "Utilities/Editor=nxedit" "Utilities/Files=nxfiles" "Utilities/Paint=nxpaint" "Other/Web=nxweb" "Utilities/Monitor=nxmon" "Utilities/Network=nxnet" "Utilities/Settings=nxsettings" > "$OUT/share/nxapps.20-apps"
# two pictures for the viewer and for its test (tools/make_test_pictures.py made them)
cp "$HERE"/pictures/picture-*.png "$HERE"/pictures/picture-*.jpg "$OUT/share/" 2>/dev/null || true
# the Start menu's last folder: the desktop over again, and the machine stopped
printf '%s\n' "System/Restart the desktop=setsid nx restart ask" "System/Save and shut down=setsid nxoff save" "System/Shut down=setsid nxoff" > "$OUT/share/nxapps.90-system"
cp "$HERE/nxoff" "$OUT/bin/nxoff"
chmod +x "$OUT/bin/nxoff"
# what opens what (open.c, docs/system/open.md): a kind a line, its endings, its first bytes, its program;
# another program's build adds its own nxopen.* file, as it adds its line to the Start menu
cp "$HERE/nxopen.conf" "$OUT/share/nxopen.20-apps"
cp "$HERE/nxopen-archive" "$HERE/nxopen-run" "$OUT/bin/"
chmod +x "$OUT/bin/nxopen-archive" "$OUT/bin/nxopen-run"
# games with the other computers (nxgame, docs/system/lan.md): a folder of the menu's Games
tr -d '\r' < "$HERE/nxgame" > "$OUT/bin/nxgame"
chmod +x "$OUT/bin/nxgame"
printf '%s\n' "Games/Network/Quake: host a game=nxgame quake host" "Games/Network/Quake: join a game=nxgame quake join" \
    "Games/Network/Quake III: host a game=nxgame quake3 host" "Games/Network/Quake III: join a game=nxgame quake3 join" \
    "Games/Network/OpenTTD: host a game=nxgame openttd host" "Games/Network/OpenTTD: join a game=nxgame openttd join" \
    "Games/Network/Doom: network game=nxgame doom host" > "$OUT/share/nxapps.75-network"
# the browser's pages: its home lists the sites of web/sites.txt (the VRChat world can ask for
# no others, so its builder reads the same file)
# (all in /usr/share: the image builder adds files to folders the ROM has, and makes none)
for f in "$HERE"/web/*.html "$HERE"/web/*.css; do cp "$f" "$OUT/share/web-$(basename "$f")"; done
python3 "$HERE/web/home.py" "$HERE/web/sites.txt" > "$OUT/share/web-index.html"
ls -l "$OUT/bin"/nxedit "$OUT/bin"/nxfiles "$OUT/bin"/nxpaint "$OUT/bin"/nxview "$OUT/bin"/nxsettings "$OUT/bin"/nxmon
