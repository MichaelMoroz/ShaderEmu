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
OUT=${DOOM_OUT:-$REPO/build/images/linux/root}	# DOOM_OUT, DOOM_BUILD: a private image's overlay, a build folder of its own
BUILD=$WORK/src/${DOOM_BUILD:-doom-build}
WAD=${DOOM_WAD:-https://archive.org/download/DoomsharewareEpisode/doom.ZIP}   # a zip holding the IWAD
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

mkdir -p "$BUILD" "$OUT/usr/bin" "$OUT/usr/share"
# Optimised as one program: the small functions Doom calls everywhere (FixedMul) end up inline.
# No position-independent code: every use of a global would go through a table.
# -O3 with these two is 3% fewer instructions a tic and 11% fewer a frame than -O2 (docs/games/doom.md).
export OPT="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -flto -fno-pie"
# What played demos ran, for the compiler (linux/userland/pgo/gcov.c; docs/games/tdawn.md, "What the compiler is told"):
# DOOM_PROFILE=count builds the program that counts, in a folder of its own; otherwise doom.profile's counts.
PGO=""; COUNTS=""; SUM=""
if [ "${DOOM_PROFILE:-}" = count ]; then
    BUILD=$WORK/src/doom-count-build; mkdir -p "$BUILD"
    mkdir -p "$WORK/src/nogcov" && riscv32-linux-ar rc "$WORK/src/nogcov/libgcov.a"
    PGO="-fprofile-generate -fprofile-update=single"; COUNTS="-L$WORK/src/nogcov"
elif [ -f "$HERE/doom.profile" ] && [ "${DOOM_PROFILE:-}" != off ]; then
    python3 "$REPO/linux/tdawn/profile.py" put "$HERE/doom.profile" "$BUILD"
    PGO="-fprofile-use -fprofile-partial-training -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch"
    SUM=$(md5sum < "$HERE/doom.profile")
fi
export OPT="$OPT $PGO"
cd "$BUILD"
# objects made with other flags (or other counts) are made again (the loop below only looks at dates)
[ "$(cat flags 2>/dev/null)" = "$OPT $SUM" ] || { rm -f *.o; echo "$OPT $SUM" > flags; }
[ "${DOOM_PROFILE:-}" = count ] && rv32-cc -O2 -fno-pie -Wall -c "$REPO/linux/userland/pgo/gcov.c" -o zz_gcov.o
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
# The search for a point's subsector is doom_fast.c's, and the tables of Doom's own renderer
# are made when that renderer is wanted (doom_gl.c).
sed 's/^R_PointInSubsector$/R_PointInSubsector_port/; s/^void R_InitTextureMapping (void)$/void R_InitTextureMapping_port (void)/' "$DOOM/r_main.c" > r_main_ours.c
grep -q '^R_PointInSubsector_port$' r_main_ours.c || { echo "r_main.c: R_PointInSubsector was not found"; exit 1; }
grep -q '^void R_InitTextureMapping_port (void)$' r_main_ours.c || { echo "r_main.c: R_InitTextureMapping was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -DR_RenderPlayerView=R_RenderPlayerView_soft -I"$DOOM" r_main_ours.c -o r_main.o
rm r_main_ours.c
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
# A frame's drawing waits for the GPU to have drawn the frame before (doom_video.c's D_Display).
# The melt's wait for the clock ends the machine's pass instead of spinning (doom_video.c's doom_wait).
sed 's/^void D_Display (void)$/void D_Display_game (void)/; s/^\t    tics = nowtime - wipestart;$/& if (!tics) doom_wait();/; 1i void doom_wait(void);' "$DOOM/d_main.c" > d_main_ours.c
grep -q '^void D_Display_game (void)$' d_main_ours.c || { echo "d_main.c: D_Display was not found"; exit 1; }
grep -q 'if (!tics) doom_wait();$' d_main_ours.c || { echo "d_main.c: the melt's wait was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -DG_Ticker=G_Ticker_counted -DHU_Drawer=HU_Drawer_kept -DHU_Erase=HU_Erase_kept -Dwipe_StartScreen=wipe_StartScreen_ours -Dwipe_EndScreen=wipe_EndScreen_ours -Dwipe_ScreenWipe=wipe_ScreenWipe_ours -I"$DOOM" d_main_ours.c -o d_main.o
rm d_main_ours.c
# A tic's move is made by way of doom_video.c, which has a holodeck's sitter aim by a hand.
# The wait for the next tic's time asked the clock in a loop, and the clock moves between the
# machine's passes: the pass ends there instead (doom_video.c's doom_idle).
sed '/while (lowtic < gametic\/ticdup + counts)/,/NetUpdate ();/ s/NetUpdate ();/doom_idle(); &/; 1i void doom_idle(void);' "$DOOM/d_net.c" > d_net_ours.c
[ "$(grep -c 'doom_idle(); NetUpdate ();' d_net_ours.c)" = 1 ] || { echo "d_net.c: the wait for a tic was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -DG_Ticker=G_Ticker_counted -DG_BuildTiccmd=G_BuildTiccmd_seat -I"$DOOM" d_net_ours.c -o d_net.o
rm d_net_ours.c
# The view is made on a worker core from what it is told (doom_gl.c): the game says which things
# it changes (a state, a place, a height), puts into a sector's list or takes out (doom_fast.c).
TOLD='1i struct mobj_s; void doom_thing_changed(struct mobj_s *thing); void doom_thing_linked(struct mobj_s *thing); void doom_thing_gone(struct mobj_s *thing);'
sed "$TOLD"'
/^P_SetMobjState$/,/^{$/ s/^{$/{ doom_thing_changed(mobj);/
/^void P_ZMovement (mobj_t\* mo)$/,/^{$/ s/^{$/{ doom_thing_changed(mo);/
/^void P_RemoveMobj (mobj_t\* mobj)$/,/^{$/ s/^{$/{ doom_thing_gone(mobj);/' "$DOOM/p_mobj.c" > p_mobj_ours.c
[ "$(tail -n +2 p_mobj_ours.c | grep -c '^{ doom_thing_\(changed(mobj)\|changed(mo)\|gone(mobj)\);$')" = 3 ] || { echo "p_mobj.c: a thing's state, height and end were not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_mobj_ours.c -o p_mobj.o
rm p_mobj_ours.c
sed "$TOLD"'
/^boolean P_ThingHeightClip (mobj_t\* thing)$/,/^{$/ s/^{$/{ doom_thing_changed(thing);/' "$DOOM/p_map.c" > p_map_ours.c
[ "$(tail -n +2 p_map_ours.c | grep -c '^{ doom_thing_changed(thing);$')" = 1 ] || { echo "p_map.c: P_ThingHeightClip was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_map_ours.c -o p_map.o
rm p_map_ours.c
# (a hit pushes a thing that may be at rest and asleep: doom_fast.c's P_RunThinkers)
sed "$TOLD"'
/^P_DamageMobj$/,/^{$/ s/^{$/{ doom_thing_changed(target);/' "$DOOM/p_inter.c" > p_inter_ours.c
[ "$(tail -n +2 p_inter_ours.c | grep -c '^{ doom_thing_changed(target);$')" = 1 ] || { echo "p_inter.c: P_DamageMobj was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_inter_ours.c -o p_inter.o
rm p_inter_ours.c
# Parts of the game written again for this machine (doom_fast.c): the sight check, and the
# marks that say a line has been visited, which leave the lines themselves.
rv32-cc $OPT -w -c -fsigned-char -DP_CheckSight=P_CheckSight_port -I"$DOOM" "$DOOM/p_sight.c" -o p_sight.o
sed 's/ld->validcount/doom_line_mark[ld - lines]/g; s/^P_Block\(Lines\|Things\)Iterator$/&_port/; 1i extern int doom_line_mark[];
'"$TOLD"'
/^P_SetThingPosition (mobj_t\* thing)$/,/^{$/ s/^{$/{ doom_thing_linked(thing);/' "$DOOM/p_maputl.c" > p_maputl_ours.c
[ "$(grep -c '^{ doom_thing_linked(thing);$' p_maputl_ours.c)" = 1 ] || { echo "p_maputl.c: P_SetThingPosition was not found"; exit 1; }
[ "$(grep -c 'doom_line_mark\[ld - lines\]' p_maputl_ours.c)" = 2 ] || { echo "p_maputl.c: the line marks were not found"; exit 1; }
[ "$(grep -c '^P_Block\(Lines\|Things\)Iterator_port$' p_maputl_ours.c)" = 2 ] || { echo "p_maputl.c: the two iterators were not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_maputl_ours.c -o p_maputl.o
rm p_maputl_ours.c
sed 's/^P_LookForPlayers$/P_LookForPlayers_port/' "$DOOM/p_enemy.c" > p_enemy_ours.c
grep -q '^P_LookForPlayers_port$' p_enemy_ours.c || { echo "p_enemy.c: P_LookForPlayers was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_enemy_ours.c -o p_enemy.o
rm p_enemy_ours.c
sed 's/^void P_RunThinkers (void)$/void P_RunThinkers_port (void)/; s/^void P_InitThinkers (void)$/void P_InitThinkers_port (void)/' "$DOOM/p_tick.c" > p_tick_ours.c
grep -q '^void P_RunThinkers_port' p_tick_ours.c || { echo "p_tick.c: P_RunThinkers was not found"; exit 1; }
grep -q '^void P_InitThinkers_port' p_tick_ours.c || { echo "p_tick.c: P_InitThinkers was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_tick_ours.c -o p_tick.o
rm p_tick_ours.c
# The kept level is told what the game changes (doom_gl.c) instead of looking at every sector
# and wall every frame: a floor or ceiling that moves (all through T_MovePlane), a floor's
# picture, a sector's light, a wall's picture (a switch, its going back, a sliding door).
# The counts are every such write the source has outside loading a level or a saved game.
told() {	# FILE CALL COUNT EXPRESSION [A LINE THAT MUST BE THERE]
    sed "1i void doom_sector_changed(void *sector); extern int doom_walls_changed;
$4" "$DOOM/$1.c" > "$1_ours.c"
    [ "$(tail -n +2 "$1_ours.c" | grep -c "$2")" = "$3" ] || { echo "$1.c: its $3 writes were not found"; exit 1; }
    [ -z "${5:-}" ] || grep -qx "$5" "$1_ours.c" || { echo "$1.c: $5 was not found"; exit 1; }
    rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" "$1_ours.c" -o "$1.o"
    rm "$1_ours.c"
}
told p_floor 'doom_sector_changed(' 3 's/^T_MovePlane$/T_MovePlane_port/; s/\(floor->sector\|sec\)->floorpic = /doom_sector_changed(\1), &/' T_MovePlane_port
told p_plats 'doom_sector_changed(' 2 's/sec->floorpic = /doom_sector_changed(sec), &/'
told p_lights 'doom_sector_changed(' 12 's/\(\(flick\|flash\|g\)-> *sector\|sector\)-> *lightlevel [-+]\?= /doom_sector_changed(\1), &/'
told p_switch 'doom_walls_changed = 1' 3 's/sides\[line->sidenum\[0\]\]\.\(top\|mid\|bottom\)texture = /doom_walls_changed = 1, &/'
told p_spec 'doom_walls_changed = 1' 3 's/sides\[buttonlist\[i\]\.line->sidenum\[0\]\]\.\(top\|mid\|bottom\)texture =$/doom_walls_changed = 1, &/'
told p_doors 'doom_walls_changed = 1' 6 's/sides\[door->line->sidenum\[[01]\]\]\.midtexture =/doom_walls_changed = 1, &/'
# The message at the top of the view stays on the screen while it says the same (doom_gl.c).
sed 's/^static hu_stext_t	w_message;/hu_stext_t w_message;/; s/^static boolean		message_on;/boolean message_on;/' "$DOOM/hu_stuff.c" > hu_stuff_ours.c
[ "$(grep -c '^hu_stext_t w_message;\|^boolean message_on;' hu_stuff_ours.c)" = 2 ] || { echo "hu_stuff.c: the message was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" hu_stuff_ours.c -o hu_stuff.o
rm hu_stuff_ours.c
# Names are looked up in tables and a sprite's size is read from its first bytes (doom_fast.c).
sed 's/^int W_CheckNumForName (char\* name)$/int W_CheckNumForName_port (char* name)/' "$DOOM/w_wad.c" > w_wad_ours.c
grep -q '^int W_CheckNumForName_port' w_wad_ours.c || { echo "w_wad.c: W_CheckNumForName was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" w_wad_ours.c -o w_wad.o
rm w_wad_ours.c
sed 's/^int\tR_CheckTextureNumForName (char \*name)$/int R_CheckTextureNumForName_port (char *name)/; s/^void R_InitSpriteLumps (void)$/void R_InitSpriteLumps_port (void)/' "$DOOM/r_data.c" > r_data_ours.c
[ "$(grep -c '^int R_CheckTextureNumForName_port\|^void R_InitSpriteLumps_port' r_data_ours.c)" = 2 ] || { echo "r_data.c: the texture names and sprite sizes were not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" r_data_ours.c -o r_data.o
rm r_data_ours.c
sed 's/^void P_GroupLines (void)$/void P_GroupLines_port (void)/' "$DOOM/p_setup.c" > p_setup_ours.c
grep -q '^void P_GroupLines_port (void)$' p_setup_ours.c || { echo "p_setup.c: P_GroupLines was not found"; exit 1; }
rv32-cc $OPT -w -c -fsigned-char -I"$DOOM" p_setup_ours.c -o p_setup.o
rm p_setup_ours.c
rv32-cc $OPT -Wall -c -fsigned-char -I"$DOOM" -I"$REPO/programs/mc" "$HERE/doom_fast.c" -o doom_fast.o
rv32-cc $OPT -Wall -Wno-unused -c -fsigned-char -I"$DOOM" -I"$REPO/linux/userland" "$HERE/doom_sound.c" -o doom_sound.o
# What a worker core writes while the first runs a tic is compiled apart, a variable a section
# and each on 16 bytes of its own: the view (doom_gl.c), the GPU's library and the workers' (mcw, mcwx)
# (docs/machine/multicore.md: two cores must not store to the same 16 bytes). They come last in the
# link, with 16 bytes of nothing after them in each kind of section: the libraries' follow.
APART="${OPT/-flto /} -fno-common -fdata-sections"
ALIGN16="--set-section-alignment .data*=16 --set-section-alignment .sdata*=16 --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16"
rv32-cc $APART -Wall -c $GLINC -I"$MW/src/include" "$REPO/programs/linux/gles.c" -o gles.o
rv32-cc $APART -Wall -Wno-unused -c -fsigned-char $GLINC -I"$DOOM" -I"$REPO/programs/mc" "$HERE/doom_gl.c" -o doom_gl.o
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -w -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcw.c" -o mcw.o
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -w -c -I"$REPO/programs/mc" "$REPO/programs/mc/mcwx.c" -o mcwx.o
echo 'char se_pad_d[16] __attribute__((aligned(16))) = {1}; char se_pad_b[16] __attribute__((aligned(16)));' > zz_pad.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=0 -c zz_pad.c -o zz_pad_large.o
sed 's/se_pad_/se_pad_s/g' zz_pad.c > zz_pads.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=64 -c zz_pads.c -o zz_pad_small.o
rm zz_pad.c zz_pads.c
for o in gles.o doom_gl.o mcw.o mcwx.o zz_pad_large.o zz_pad_small.o; do riscv32-linux-objcopy $ALIGN16 "$o"; done
rv32-cc $OPT -Wall -c $GLINC -I"$MW/src/include" -I"$REPO/programs/mc" "$HERE/doom_video.c" -o doom_video.o
rm -f doom_fps.o
rv32-cc $OPT -w $(ls *.o | grep -vx -e 'zz_pad_.*' -e doom_gl.o -e gles.o -e mcw.o -e mcwx.o -e zz_gcov.o) $COUNTS $( [ -f zz_gcov.o ] && echo zz_gcov.o ) doom_gl.o gles.o mcw.o mcwx.o zz_pad_large.o zz_pad_small.o "$MW/src/lib/libnano-X.a" -lm -o doom
riscv32-linux-strip -o "$OUT/usr/bin/doom.bin" doom
# the image builder adds files to directories the image has, so the data sits in /usr/share
# its settings and saved games go to the folder the host keeps (docs/system/persist.md)
printf '#!/bin/sh
mkdir -p /persistence/doom && cd /persistence/doom || exit 1
HOME=/persistence/doom DOOMWADDIR=/usr/share exec doom.bin "$@"
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
# a WAD anybody opens is played (docs/system/open.md): a whole game by itself, a patch over the shareware one
printf '%s\n' 'doom-game - IWAD doom -iwad %s' 'doom-patch wad PWAD doom -file %s' > "$OUT/usr/share/nxopen.50-doom"
# the Start menu lists what the nxapps.* files name (nxbar.c)
echo "Games/Doom=doom" > "$OUT/usr/share/nxapps.50-doom"
ls -l "$OUT/usr/bin/doom.bin" "$OUT/usr/share/doom1.wad"
