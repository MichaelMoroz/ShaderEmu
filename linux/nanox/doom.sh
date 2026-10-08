#!/usr/bin/env bash
# Builds the Doom that comes with Microwindows (src/contrib/doom, a Nano-X client) for the
# image, and fetches the shareware game data, which is not kept in this repository.
#   wsl -- bash /mnt/c/.../linux/nanox/doom.sh        (after build.sh)
# In the guest: nano-X & doom      (-1, -2, -3: the window's scale; -timedemo demo1 measures)
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
# Optimised as one program: the small functions Doom calls everywhere (FixedMul) end up inline.
# No position-independent code: every use of a global would go through a table.
export OPT="-O2 -flto -fno-pie"
cd "$WORK/src/doom-build"
# Doom assumes char is signed (its tables end with -1); on RISC-V it is not unless asked
# (i_sound.c is the port's sound, which mixed for a sound server: doom_sound.c takes its place)
SOURCES=$(cd "$DOOM" && ls *.c | grep -v _sdl | grep -vx i_sound.c)
rm -f i_sound.o
for f in $SOURCES; do
    [ "${f%.c}.o" -nt "$DOOM/$f" ] || echo "$f"
done | xargs -r -P "$(nproc)" -I{} sh -c \
    'rv32-cc $OPT -w -c -fsigned-char -DALWAYS=1 -DIPPORT_USERRESERVED=5000 -DPACKAGE=\"doom\" -DVERSION=\"1.10\" -I"$1" -I"$2/src/include" "$1/$0" -o "${0%.c}.o"' {} "$DOOM" "$MW"
# The sprite name table has no end marker and is counted up to the first zero after it:
# count no further than the table (the source tree itself is left as it is).
sed 's/while (\*check != NULL)/while (check - namelist < NUMSPRITES \&\& *check != NULL)/' "$DOOM/r_things.c" > r_things_bounded.c
grep -q 'check - namelist < NUMSPRITES' r_things_bounded.c || { echo "r_things.c: the sprite count loop was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" -include info.h r_things_bounded.c -o r_things.o
rm r_things_bounded.c
# The shareware data's demos are version 1.9 (109), which plays the same as this source's 1.10.
sed 's/if ( \*demo_p++ != VERSION_NUM)/if ( *demo_p++ != VERSION_NUM \&\& demo_p[-1] != 109)/' "$DOOM/g_game.c" > g_game_demo.c
grep -q 'demo_p\[-1\] != 109' g_game_demo.c || { echo "g_game.c: the demo version check was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" g_game_demo.c -o g_game.o
rm g_game_demo.c
# The frame goes to the GPU (doom_video.c): the port's frame and palette functions under other
# names, as the fallback, and its window handle made reachable.
# The window's size is ours to choose unless -1, -2 or -3 says (doom_video.c).
{ sed 's/^    w = SCREENWIDTH \* multiply;$/    multiply = doom_window_scale(multiply, M_CheckParm("-1") || M_CheckParm("-2") || M_CheckParm("-3")); w = SCREENWIDTH * multiply;/' "$DOOM/i_video.c"
  echo 'GR_WINDOW_ID doom_window(void) { return win; }'; } > i_video_ours.c
grep -q 'doom_window_scale' i_video_ours.c || { echo "i_video.c: the window's size was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -DALWAYS=1 -DI_FinishUpdate=I_FinishUpdate_port -DI_SetPalette=I_SetPalette_port -DI_InitGraphics=I_InitGraphics_port -DI_StartTic=I_StartTic_port    -I"$DOOM" -I"$MW/src/include" i_video_ours.c -o i_video.o
rm i_video_ours.c
# The 3D view goes to the GPU too (doom_gl.c): the renderer's entry points under other names,
# which ours call when the GPU is not drawing the view.
GLINC="-idirafter $REPO/programs/linux/include"
rv32-cc $OPT -w -c -fsigned-char -DR_RenderPlayerView=R_RenderPlayerView_soft -I"$DOOM" "$DOOM/r_main.c" -o r_main.o
rv32-cc $OPT -w -c -fsigned-char -DR_StoreWallRange=R_StoreWallRange_soft -I"$DOOM" "$DOOM/r_segs.c" -o r_segs.o
sed 's/^void R_Subsector (int num)/void R_Subsector_soft (int num)/' "$DOOM/r_bsp.c" > r_bsp_ours.c
grep -q 'R_Subsector_soft' r_bsp_ours.c || { echo "r_bsp.c: R_Subsector was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" r_bsp_ours.c -o r_bsp.o
rm r_bsp_ours.c
rv32-cc $OPT -w -c -fsigned-char -DV_DrawPatch=V_DrawPatch_cpu -I"$DOOM" "$DOOM/v_video.c" -o v_video.o
# The port divides fixed-point numbers as doubles, which this machine has no hardware for:
# ours (doom_gl.c) divides integers, as the original did.
sed 's/^FixedDiv2$/FixedDiv2_double/' "$DOOM/m_fixed.c" > m_fixed_ours.c
grep -q '^FixedDiv2_double$' m_fixed_ours.c || { echo "m_fixed.c: FixedDiv2 was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" m_fixed_ours.c -o m_fixed.o
rm m_fixed_ours.c
# A status bar number is drawn again every frame: only when it has changed, or all is redrawn.
sed 's/^    n->oldnum = \*n->num;/    if (n->oldnum == *n->num \&\& !refresh) return; n->oldnum = *n->num;/' "$DOOM/st_lib.c" > st_lib_ours.c
grep -q 'oldnum == \*n->num && !refresh' st_lib_ours.c || { echo "st_lib.c: the number drawing was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" st_lib_ours.c -o st_lib.o
rm st_lib_ours.c
# The time comes from the machine's clock word (doom_video.c); the port's clock is the fallback.
sed 's/^int  I_GetTime (void)$/int I_GetTime_port (void)/' "$DOOM/i_system.c" > i_system_ours.c
grep -q '^int I_GetTime_port' i_system_ours.c || { echo "i_system.c: I_GetTime was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -DALWAYS=1 -I"$DOOM" -I"$MW/src/include" i_system_ours.c -o i_system.o
rm i_system_ours.c
# The game's tics are counted apart from drawing (doom_video.c prints both with the frame rate).
rv32-cc $OPT -w -c -fsigned-char -DG_Ticker=G_Ticker_counted -DHU_Drawer=HU_Drawer_kept -Dwipe_StartScreen=wipe_StartScreen_ours -Dwipe_EndScreen=wipe_EndScreen_ours -Dwipe_ScreenWipe=wipe_ScreenWipe_ours -I"$DOOM" "$DOOM/d_main.c" -o d_main.o
rv32-cc $OPT -w -c -fsigned-char -DG_Ticker=G_Ticker_counted -I"$DOOM" "$DOOM/d_net.c" -o d_net.o
# Parts of the game written again for this machine (doom_fast.c): the sight check, and the
# marks that say a line has been visited, which leave the lines themselves.
rv32-cc $OPT -w -c -fsigned-char -DP_CheckSight=P_CheckSight_port -I"$DOOM" "$DOOM/p_sight.c" -o p_sight.o
sed 's/ld->validcount/doom_line_mark[ld - lines]/g; 1i extern int doom_line_mark[];' "$DOOM/p_maputl.c" > p_maputl_ours.c
[ "$(grep -c 'doom_line_mark\[ld - lines\]' p_maputl_ours.c)" = 2 ] || { echo "p_maputl.c: the line marks were not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_maputl_ours.c -o p_maputl.o
rm p_maputl_ours.c
sed 's/^P_LookForPlayers$/P_LookForPlayers_port/' "$DOOM/p_enemy.c" > p_enemy_ours.c
grep -q '^P_LookForPlayers_port$' p_enemy_ours.c || { echo "p_enemy.c: P_LookForPlayers was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_enemy_ours.c -o p_enemy.o
rm p_enemy_ours.c
sed 's/^void P_RunThinkers (void)$/void P_RunThinkers_port (void)/' "$DOOM/p_tick.c" > p_tick_ours.c
grep -q '^void P_RunThinkers_port' p_tick_ours.c || { echo "p_tick.c: P_RunThinkers was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_tick_ours.c -o p_tick.o
rm p_tick_ours.c
# The message at the top of the view stays on the screen while it says the same (doom_gl.c).
sed 's/^static hu_stext_t	w_message;/hu_stext_t w_message;/; s/^static boolean		message_on;/boolean message_on;/' "$DOOM/hu_stuff.c" > hu_stuff_ours.c
[ "$(grep -c '^hu_stext_t w_message;\|^boolean message_on;' hu_stuff_ours.c)" = 2 ] || { echo "hu_stuff.c: the message was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" hu_stuff_ours.c -o hu_stuff.o
rm hu_stuff_ours.c
rv32-cc $OPT -Wall -c -fsigned-char -I"$DOOM" "$HERE/doom_fast.c" -o doom_fast.o
rv32-cc $OPT -Wall -Wno-unused -c -fsigned-char -I"$DOOM" -I"$REPO/linux/userland" "$HERE/doom_sound.c" -o doom_sound.o
rv32-cc $OPT -Wall -c $GLINC -I"$MW/src/include" "$REPO/programs/linux/gles.c" -o gles.o
rv32-cc $OPT -Wall -Wno-unused -c -fsigned-char $GLINC -I"$DOOM" "$HERE/doom_gl.c" -o doom_gl.o
rv32-cc $OPT -Wall -c $GLINC -I"$MW/src/include" "$HERE/doom_video.c" -o doom_video.o
rm -f doom_fps.o
rv32-cc $OPT -w *.o "$MW/src/lib/libnano-X.a" -lm -o doom
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
echo "Games/Doom=doom" > "$OUT/usr/share/nxapps.50-doom"
ls -l "$OUT/usr/bin/doom.bin" "$OUT/usr/share/doom1.wad"
