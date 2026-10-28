#!/usr/bin/env bash
# Builds Quake III Arena (ioquake3's source; the game, the client's game and the menus linked
# in, a renderer and a sound layer of our own) for the image, and bakes one level of the
# game's free demo into the two files it runs from (docs/games/quake3.md).
#   wsl -- bash /mnt/c/.../linux/quake3/build.sh        (after linux/nanox/build.sh)
# In the guest: nano-X -p & quake3
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=${SE_MW:-$WORK/src/microwindows}
Q=${QUAKE3_SRC:-$WORK/src/ioq3}
BUILD=${QUAKE3_BUILD:-$WORK/src/quake3-build}
DATA=${QUAKE3_DATA:-$WORK/src/quake3-data}
OUT=${QUAKE3_OUT:-$REPO/build/images/linux/root}
PROGRAMS=${SE_PROGRAMS:-$REPO/programs}
MEDIA=${QUAKE3_MEDIA:-$WORK/src/libmedia}    # libmedia.a (linux/libmedia/build.sh)
COMMIT=87a99d18
DEMO=${QUAKE3_DEMO:-https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake3/linux/linuxq3ademo-1.11-6.x86.gz.sh}
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
[ -f "$MEDIA/libmedia.a" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }

if [ ! -d "$Q/.git" ]; then
    echo "== ioquake3 $COMMIT"
    git clone -q https://github.com/ioquake/ioq3.git "$Q"
fi
# the tree is ioquake3's commit plus our patch, put there again on every build
# (QUAKE3_TREE=keep: the tree as it stands, for writing that patch)
if [ "${QUAKE3_TREE:-}" != keep ] && ! { [ "$(git -C "$Q" rev-parse HEAD)" = "$(git -C "$Q" rev-parse "$COMMIT^{commit}")" ] &&
        [ -z "$(git -C "$Q" status --porcelain --untracked-files=all | grep '^??')" ] && git -C "$Q" diff -- code | cmp -s - "$HERE/quake3.patch"; }; then
    # (a tree that is that already is left alone: its files keep their times, and only what changed is compiled)
    git -C "$Q" checkout -q -f "$COMMIT" && git -C "$Q" clean -q -fd
    git -C "$Q" apply "$HERE/quake3.patch"
fi

C=$Q/code
ENGINE="qcommon/cm_load qcommon/cm_patch qcommon/cm_polylib qcommon/cm_test qcommon/cm_trace qcommon/cmd
    qcommon/common qcommon/cvar qcommon/files qcommon/md4 qcommon/md5 qcommon/msg qcommon/net_chan qcommon/net_ip
    qcommon/huffman qcommon/q_math qcommon/q_shared qcommon/unzip qcommon/ioapi qcommon/vm qcommon/vm_interpreted
    server/sv_bot server/sv_client server/sv_ccmds server/sv_game server/sv_init server/sv_main server/sv_net_chan
    server/sv_snapshot server/sv_world
    client/cl_cgame client/cl_cin client/cl_console client/cl_input client/cl_keys client/cl_main client/cl_net_chan
    client/cl_parse client/cl_scrn client/cl_ui client/cl_avi
    sys/sys_unix
    botlib/be_aas_bspq3 botlib/be_aas_cluster botlib/be_aas_debug botlib/be_aas_entity botlib/be_aas_file
    botlib/be_aas_main botlib/be_aas_move botlib/be_aas_optimize botlib/be_aas_reach botlib/be_aas_route
    botlib/be_aas_routealt botlib/be_aas_sample botlib/be_ai_char botlib/be_ai_chat botlib/be_ai_gen botlib/be_ai_goal
    botlib/be_ai_move botlib/be_ai_weap botlib/be_ai_weight botlib/be_ea botlib/be_interface botlib/l_crc
    botlib/l_libvar botlib/l_log botlib/l_memory botlib/l_precomp botlib/l_script botlib/l_struct"
ZLIB="adler32 crc32 inffast inflate inftrees zutil"
GAME="g_main ai_chat ai_cmd ai_dmnet ai_dmq3 ai_main ai_team ai_vcmd bg_misc bg_pmove bg_slidemove g_active g_arenas
    g_bot g_client g_cmds g_combat g_items g_mem g_misc g_missile g_mover g_session g_spawn g_svcmds g_target g_team
    g_trigger g_utils g_weapon g_syscalls"
CGAME="cgame/cg_main game/bg_misc game/bg_pmove game/bg_slidemove cgame/cg_consolecmds cgame/cg_draw
    cgame/cg_drawtools cgame/cg_effects cgame/cg_ents cgame/cg_event cgame/cg_info cgame/cg_localents cgame/cg_marks
    cgame/cg_particles cgame/cg_players cgame/cg_playerstate cgame/cg_predict cgame/cg_scoreboard
    cgame/cg_servercmds cgame/cg_snapshot cgame/cg_view cgame/cg_weapons cgame/cg_syscalls"
UI="q3_ui/ui_main game/bg_misc q3_ui/ui_addbots q3_ui/ui_atoms q3_ui/ui_cdkey q3_ui/ui_cinematics q3_ui/ui_confirm
    q3_ui/ui_connect q3_ui/ui_controls2 q3_ui/ui_credits q3_ui/ui_demo2 q3_ui/ui_display q3_ui/ui_gameinfo
    q3_ui/ui_ingame q3_ui/ui_loadconfig q3_ui/ui_menu q3_ui/ui_mfield q3_ui/ui_mods q3_ui/ui_network q3_ui/ui_options
    q3_ui/ui_playermodel q3_ui/ui_players q3_ui/ui_playersettings q3_ui/ui_preferences q3_ui/ui_qmenu
    q3_ui/ui_removebots q3_ui/ui_serverinfo q3_ui/ui_servers2 q3_ui/ui_setup q3_ui/ui_sound q3_ui/ui_sparena
    q3_ui/ui_specifyserver q3_ui/ui_splevel q3_ui/ui_sppostgame q3_ui/ui_spskill q3_ui/ui_startserver q3_ui/ui_team
    q3_ui/ui_teamorders q3_ui/ui_video ui/ui_syscalls"
OURS="sys_shaderemu tr_shaderemu snd_shaderemu server_shaderemu client_shaderemu"

# The flags are Quake's (linux/quake/build.sh says why each): floats only, a variable a section
# of its own on 16 bytes, and the game's sine, cosine and square root in floats (fmath.h).
GLES="-DSEGL_FLOAT -DMAX_TEXTURES=64"
GLINC="-idirafter $PROGRAMS/linux/include -I$MW/src/include"
FPU="-fno-math-errno -fsingle-precision-constant"
# GCC inlining more (gpu_vm inlines 3 times its compiler's threshold): fewer calls, fewer prologue saves, which are stores
# (docs/plans/riscv-backport-plan.md, 2b). On rvc_cpu Quake's track 10 to 1200 2.1% fewer instructions on the first core,
# Quake III's 600 frames 1.9% fewer on all cores and 2.4% less guest time; Doom and the NES gained nothing.
OPT="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops --param max-inline-insns-auto=60 --param max-inline-insns-single=400 --param inline-unit-growth=80"
DEFS="-DSTANDALONE -DBOTLIB -DNO_VM_COMPILED -DNDEBUG -DSHADEREMU -DPRODUCT_VERSION=\"1.36-shaderemu\" ${QUAKE3_DEFINES:-}"
CFLAGS="$OPT $FPU -fno-pie -fno-common -fdata-sections -fsigned-char -fno-strict-aliasing -w $DEFS -include $HERE/fmath.h $GLINC -I$C/thirdparty/zlib-1.3.1 -I$PROGRAMS/mc -I$REPO/linux/libmedia -I$HERE -I$C"
ALIGN16="--set-section-alignment .data*=16 --set-section-alignment .sdata*=16 --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16"
export CFLAGS C HERE BUILD ALIGN16
mkdir -p "$BUILD/engine" "$BUILD/game" "$BUILD/cgame" "$BUILD/ui" && cd "$BUILD"

# one object a source, made again when the source, the patch, this script or a header of ours is newer
compile() {   # folder, then sources (paths without .c)
    local dir=$1; shift
    printf '%s\n' "$@" | xargs -P "$(nproc)" -I{} sh -c 'o='"$dir"'/$(basename {}).o; [ "$o" -nt {}.c ] && [ "$o" -nt "$HERE/quake3.patch" ] && [ "$o" -nt "$HERE/build.sh" ] && [ "$o" -nt "$HERE/fmath.h" ] && [ "$o" -nt "$HERE/se_q3.h" ] && [ "$o" -nt "$HERE/se_data.h" ] && [ "$o" -nt "$HERE/gpu_shaderemu.h" ] || { rv32-cc $CFLAGS -c {}.c -o "$o" && riscv32-linux-objcopy $ALIGN16 "$o"; }'
}
compile engine $(for f in $ENGINE; do echo "$C/$f"; done) $(for f in $ZLIB; do echo "$C/thirdparty/zlib-1.3.1/$f"; done) \
    $(for f in $OURS; do echo "$HERE/$f"; done) "$REPO/linux/quake/fmath_shaderemu"
compile game $(for f in $GAME; do echo "$C/game/$f"; done)
compile cgame $(for f in $CGAME; do echo "$C/$f"; done)
compile ui $(for f in $UI; do echo "$C/$f"; done)
# The three modules were libraries with the same names in each (vmMain, dllEntry, and their own
# Com_Printf): each is linked to one object whose names stay inside it, but for those two. Its
# variables are two sections of its own, which the system layer puts back as they were when
# the module is loaded again (a library's are fresh each time it is loaded).
for m in game cgame ui; do
    cat > "$m.ld" <<EOF
SECTIONS {
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.* .srodata .srodata.*) }
    ${m}_data : { *(.data .data.* .sdata .sdata.*) }
    ${m}_bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
}
EOF
    riscv32-linux-ld -r -T "$m.ld" -o "$m.all.o" "$m"/*.o
    riscv32-linux-objcopy --keep-global-symbol=vmMain --keep-global-symbol=dllEntry "$m.all.o" "$m.o"
    riscv32-linux-objcopy --redefine-sym vmMain=${m}_vmMain --redefine-sym dllEntry=${m}_dllEntry "$m.o"
done
# gles.c in its float build, with what the renderer adds to it (gpu_shaderemu.c includes it)
rv32-cc $OPT $FPU -fno-pie -fno-common -fdata-sections -Wall -c $GLES $GLINC -I"$PROGRAMS/linux" "$HERE/gpu_shaderemu.c" -o gpu.o
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -Wall -c -I"$PROGRAMS/mc" "$PROGRAMS/mc/mcw.c" -o mcw.o
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -Wall -c -I"$PROGRAMS/mc" "$PROGRAMS/mc/mcwx.c" -o mcwx.o
printf 'char se_pad_d[16] __attribute__((aligned(16))) = {1}; char se_pad_b[16] __attribute__((aligned(16)));\n' > zz_pad.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=0 -c zz_pad.c -o zz_pad_large.o
sed 's/se_pad_/se_pad_s/g' zz_pad.c > zz_pads.c
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -msmall-data-limit=64 -c zz_pads.c -o zz_pad_small.o
for o in gpu.o mcw.o mcwx.o zz_pad_large.o zz_pad_small.o; do riscv32-linux-objcopy $ALIGN16 "$o"; done
rv32-cc -O2 $FPU -w ${QUAKE3_LINK:-} engine/*.o game.o cgame.o ui.o gpu.o mcw.o mcwx.o zz_pad_large.o zz_pad_small.o "$MW/src/lib/libnano-X.a" "$MEDIA/libmedia.a" -lm -o quake3
mkdir -p "$OUT/usr/bin" "$OUT/usr/share"
riscv32-linux-strip -o "$OUT/usr/bin/quake3.bin" quake3
riscv32-linux-nm -n quake3 > "${QUAKE3_NM:-$WORK/src/quake3.nm}"

# The demo: a shell script with an archive after line 164, and in it demoq3/pak0.pk3.
if [ ! -f "$DATA/demo/demoq3/pak0.pk3" ]; then
    echo "== the demo's data"
    mkdir -p "$DATA/demo" && cd "$DATA"
    [ -f demo.sh ] || curl -sSfL "$DEMO" -o demo.sh
    tail -n +165 demo.sh | gzip -cd | tar xf - -C demo
fi
[ "$(md5sum < "$DATA/demo/demoq3/pak0.pk3" | cut -d' ' -f1)" = 0613b3d4ef05e613a2b470571498690f ] || { echo "pak0.pk3 is not the 1.11 demo's"; exit 1; }

# One level of it, baked (bake.py): what the game asked for in a run with QUAKE3_WANTED=1 is
# wanted.txt, and jpgraw reads the demo's JPEG pictures (the build computer's own programs);
# q3pack writes them as the files the game decodes, with libmedia's encoders.
J=$C/thirdparty/jpeg-9f
if [ ! -x "$DATA/jpgraw" ] || [ "$HERE/jpgraw.c" -nt "$DATA/jpgraw" ]; then
    gcc -O2 -w -I"$J" -o "$DATA/jpgraw" "$HERE/jpgraw.c" $(ls "$J"/j*.c | grep -v 'jmemansi\|jmemname\|jmemdos\|jmemmac') -lm
fi
L=$REPO/linux/libmedia
if [ ! -x "$DATA/q3pack" ] || [ -n "$(find "$HERE/q3pack.c" "$L" -maxdepth 1 -name '*.[ch]' -newer "$DATA/q3pack")" ]; then
    gcc -O2 -w -I"$L" -I"$PROGRAMS/mc" -o "$DATA/q3pack" "$HERE/q3pack.c" \
        $(for f in m_core m_simple m_png m_png_enc m_jpeg m_jpeg_enc m_gif m_gif_enc; do echo "$L/$f.c"; done) "$L/test/mcw_host.c" -lpthread -lm
fi
if [ ! -f "$DATA/se.dat" ] || [ "$HERE/bake.py" -nt "$DATA/se.dat" ] || [ "$HERE/wanted.txt" -nt "$DATA/se.dat" ] || [ "$HERE/build.sh" -nt "$DATA/se.dat" ] ||
        [ "$DATA/q3pack" -nt "$DATA/se.dat" ]; then
    python3 "$HERE/bake.py" "$DATA/demo/demoq3/pak0.pk3" "$HERE/wanted.txt" "$DATA/pak0.pk3" "$DATA/se.dat" --jpgraw "$DATA/jpgraw" \
        --q3pack "$DATA/q3pack" ${QUAKE3_BAKE:-}
fi
# all in /usr/share: the image builder adds files to folders the ROM has, and makes none.
# The game wants its data in a folder baseq3 and writes its settings beside it: the starter makes one.
cp "$DATA/pak0.pk3" "$OUT/usr/share/quake3-pak0.pk3"
cp "$DATA/se.dat" "$OUT/usr/share/quake3-se.dat"
cat > "$OUT/usr/bin/quake3" <<'EOF'
#!/bin/sh
# what it writes (its settings) goes to the folder the host keeps (docs/system/persist.md)
mkdir -p /tmp/quake3/baseq3 /persistence/quake3 && cd /tmp/quake3 || exit 1
ln -sf /usr/share/quake3-pak0.pk3 baseq3/pak0.pk3
exec quake3.bin +set fs_basepath /tmp/quake3 +set fs_homepath /persistence/quake3 +set sv_pure 0 +set name "${USER:-player}" "$@"
EOF
chmod +x "$OUT/usr/bin/quake3"
# (from the menu: the level, and the one bot the worker core keeps up with: docs/games/quake3.md)
echo "Games/Quake III Arena=quake3" > "$OUT/usr/share/nxapps.71-quake3"
ls -l "$OUT/usr/bin/quake3.bin" "$OUT/usr/share/quake3-pak0.pk3" "$OUT/usr/share/quake3-se.dat"
