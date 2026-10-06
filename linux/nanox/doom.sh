#!/usr/bin/env bash
# Builds the Doom that comes with Microwindows (src/contrib/doom, a Nano-X client) for the
# image, and fetches the shareware game data, which is not kept in this repository.
#   wsl -- bash /mnt/c/.../linux/nanox/doom.sh        (after build.sh)
# In the guest: nano-X & doom      (-2 doubles the window; -timedemo demo1 measures)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=$WORK/src/microwindows
DOOM=$MW/src/contrib/doom
OUT=$REPO/build/images/linux/root
WAD=${DOOM_WAD:-https://archive.org/download/DoomsharewareEpisode/doom.ZIP}   # a zip holding the IWAD
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

mkdir -p "$WORK/src/doom-build" "$OUT/usr/bin" "$OUT/usr/share"
cd "$WORK/src/doom-build"
# Doom assumes char is signed (its tables end with -1); on RISC-V it is not unless asked
SOURCES=$(cd "$DOOM" && ls *.c | grep -v _sdl)
for f in $SOURCES; do
    [ "${f%.c}.o" -nt "$DOOM/$f" ] || echo "$f"
done | xargs -r -P "$(nproc)" -I{} sh -c \
    'rv32-cc -O2 -w -c -fsigned-char -DALWAYS=1 -DIPPORT_USERRESERVED=5000 -DPACKAGE=\"doom\" -DVERSION=\"1.10\" -I"$1" -I"$2/src/include" "$1/$0" -o "${0%.c}.o"' {} "$DOOM" "$MW"
# The sprite name table has no end marker and is counted up to the first zero after it:
# count no further than the table (the source tree itself is left as it is).
sed 's/while (\*check != NULL)/while (check - namelist < NUMSPRITES \&\& *check != NULL)/' "$DOOM/r_things.c" > r_things_bounded.c
grep -q 'check - namelist < NUMSPRITES' r_things_bounded.c || { echo "r_things.c: the sprite count loop was not found"; exit 1; }
rv32-cc -O2 -w -c -fsigned-char -I"$DOOM" -include info.h r_things_bounded.c -o r_things.o
rm r_things_bounded.c
# The shareware data's demos are version 1.9 (109), which plays the same as this source's 1.10.
sed 's/if ( \*demo_p++ != VERSION_NUM)/if ( *demo_p++ != VERSION_NUM \&\& demo_p[-1] != 109)/' "$DOOM/g_game.c" > g_game_demo.c
grep -q 'demo_p\[-1\] != 109' g_game_demo.c || { echo "g_game.c: the demo version check was not found"; exit 1; }
rv32-cc -O2 -w -c -fsigned-char -I"$DOOM" g_game_demo.c -o g_game.o
rm g_game_demo.c
# The frame goes to the GPU (doom_video.c): the port's frame and palette functions under other
# names, as the fallback, and its window handle made reachable.
{ cat "$DOOM/i_video.c"; echo 'GR_WINDOW_ID doom_window(void) { return win; }'; } > i_video_ours.c
rv32-cc -O2 -w -c -fsigned-char -DALWAYS=1 -DI_FinishUpdate=I_FinishUpdate_port -DI_SetPalette=I_SetPalette_port -DI_InitGraphics=I_InitGraphics_port -DI_StartTic=I_StartTic_port    -I"$DOOM" -I"$MW/src/include" i_video_ours.c -o i_video.o
rm i_video_ours.c
# The 3D view goes to the GPU too (doom_gl.c): the renderer's entry points under other names,
# which ours call when the GPU is not drawing the view.
GLINC="-idirafter $REPO/programs/linux/include"
rv32-cc -O2 -w -c -fsigned-char -DR_RenderPlayerView=R_RenderPlayerView_soft -I"$DOOM" "$DOOM/r_main.c" -o r_main.o
rv32-cc -O2 -w -c -fsigned-char -DR_StoreWallRange=R_StoreWallRange_soft -I"$DOOM" "$DOOM/r_segs.c" -o r_segs.o
sed 's/^void R_Subsector (int num)/void R_Subsector_soft (int num)/' "$DOOM/r_bsp.c" > r_bsp_ours.c
grep -q 'R_Subsector_soft' r_bsp_ours.c || { echo "r_bsp.c: R_Subsector was not found"; exit 1; }
rv32-cc -O2 -w -c -fsigned-char -I"$DOOM" r_bsp_ours.c -o r_bsp.o
rm r_bsp_ours.c
rv32-cc -O2 -w -c -fsigned-char -DV_DrawPatch=V_DrawPatch_cpu -I"$DOOM" "$DOOM/v_video.c" -o v_video.o
# The port divides fixed-point numbers as doubles, which this machine has no hardware for:
# ours (doom_gl.c) divides integers, as the original did.
sed 's/^FixedDiv2$/FixedDiv2_double/' "$DOOM/m_fixed.c" > m_fixed_ours.c
grep -q '^FixedDiv2_double$' m_fixed_ours.c || { echo "m_fixed.c: FixedDiv2 was not found"; exit 1; }
rv32-cc -O2 -w -c -fsigned-char -I"$DOOM" m_fixed_ours.c -o m_fixed.o
rm m_fixed_ours.c
# A status bar number is drawn again every frame: only when it has changed, or all is redrawn.
sed 's/^    n->oldnum = \*n->num;/    if (n->oldnum == *n->num \&\& !refresh) return; n->oldnum = *n->num;/' "$DOOM/st_lib.c" > st_lib_ours.c
grep -q 'oldnum == \*n->num && !refresh' st_lib_ours.c || { echo "st_lib.c: the number drawing was not found"; exit 1; }
rv32-cc -O2 -w -c -fsigned-char -I"$DOOM" st_lib_ours.c -o st_lib.o
rm st_lib_ours.c
rv32-cc -O2 -Wall -c $GLINC -I"$MW/src/include" "$REPO/programs/linux/gles.c" -o gles.o
rv32-cc -O2 -Wall -Wno-unused -c -fsigned-char $GLINC -I"$DOOM" "$HERE/doom_gl.c" -o doom_gl.o
rv32-cc -O2 -Wall -c $GLINC -I"$MW/src/include" "$HERE/doom_video.c" -o doom_video.o
rm -f doom_fps.o
rv32-cc *.o "$MW/src/lib/libnano-X.a" -lm -o doom
riscv32-linux-strip -o "$OUT/usr/bin/doom.bin" doom
# the image builder adds files to directories the image has, so the data sits in /usr/share
printf '#!/bin/sh
DOOMWADDIR=/usr/share exec doom.bin "$@"
' > "$OUT/usr/bin/doom"
if [ ! -f "$WORK/src/doom1.wad" ]; then
    echo "== shareware doom1.wad"
    curl -sSL "$WAD" -o "$WORK/src/doom-shareware.zip"
    python3 - "$WORK/src/doom-shareware.zip" "$WORK/src/doom1.wad" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for name in z.namelist():
    data = z.read(name)
    if data[:4] == b'IWAD':
        open(sys.argv[2], 'wb').write(data)
        break
else:
    sys.exit('no IWAD in the download: ' + ', '.join(z.namelist()))
PY
fi
cp "$WORK/src/doom1.wad" "$OUT/usr/share/doom1.wad"
# the Start menu lists what the nxapps.* files name (nxbar.c)
echo "Doom=doom" > "$OUT/usr/share/nxapps.50-doom"
ls -l "$OUT/usr/bin/doom.bin" "$OUT/usr/share/doom1.wad"
