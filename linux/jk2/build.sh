#!/usr/bin/env bash
# Builds Star Wars Jedi Knight II: Jedi Outcast's single player (OpenJK's JK2 code, GPL-2.0) as one program: the engine
# (code/ with JK2_MODE), the game with its client's game (codeJK2/) and the renderer with Ghoul2 (code/rd-vanilla) linked
# in, and a platform layer of our own (sys_shaderemu.cpp). docs/games/jk2.md.
#   JK2_HOST=1 bash linux/jk2/build.sh     the build computer's own program, with no window, no picture and no sound
#                                          (WSL; a folder of its own, $JK2_WORK or ~/jk2-host): the demo's level loads
#                                          and plays there, and its sums are the machine's to match
#   bash linux/jk2/build.sh                for the machine, with OpenTTD's C++ compiler (linux/openttd/toolchain.sh):
#                                          written, not yet built (docs/games/jk2.md, "What is left")
# Nothing of the demo is here: linux/jk2/data.py fetches it, and puts it into an image only when asked.
#   JK2_TREE=keep          the tree as it stands, for writing the patch: (cd TREE && git diff -- code codeJK2 shared lib) > linux/jk2/jk2.patch
#   JK2_OUT=DIR            where the program goes (usr/bin/jk2.bin), for the machine; the host's stays in its folder
#   JK2_JOBS=N             compilers at once (4)
#   JK2_CFLAGS=, JK2_LINK= more flags for every compile, for the link (-fsanitize=address on the host, with JK2_WORK=DIR
#                          and JK2_SRC=the tree: a build of its own)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
COMMIT=260c59c2907187af555a676fe0cc798893bf7757
JOBS=${JK2_JOBS:-4}
T0=$(date +%s); step() { echo "== [$(( $(date +%s) - T0 )) s] $*"; }

# (a player made ahead neither allocates nor frees: sys_shaderemu.cpp's __wrap_malloc and the rest; docs/games/jk2.md,
# "The players on workers")
SE_WRAP="-Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free"
if [ "${JK2_HOST:-}" = 1 ]; then
    WORK=${JK2_WORK:-$HOME/jk2-host}
    mkdir -p "$WORK"
    CC=gcc; CXX=g++; LD=ld; OBJCOPY=objcopy; NM=nm; ARCH=x86_64
    # the machine's float flags (below), so that the game computes the same: its sums are compared
    OPT="-O2 -g -fno-strict-aliasing -fno-math-errno -fsingle-precision-constant -ffp-contract=off ${JK2_CFLAGS:-}"
    LIBS="-lm $SE_WRAP ${JK2_LINK:-}"
    BUILD=$WORK/build
    OUT=$WORK
else
    source "$HERE/../openttd/toolchain.sh"      # ottd-cc, ottd-c++ ($WORK, $CXXTC)
    CC=ottd-cc; CXX=ottd-c++; T=riscv32-linux-musl
    LD=$CXXTC/bin/$T-ld; OBJCOPY=$CXXTC/bin/$T-objcopy; NM=$CXXTC/bin/$T-nm; ARCH=riscv32
    # Quake's flags (linux/quake/build.sh): floats only, a variable a section of its own on 16 bytes
    OPT="-O2 -fno-strict-aliasing -fno-math-errno -fsingle-precision-constant -fno-common -fdata-sections ${JK2_CFLAGS:-}"
    LIBS="-lm ${JK2_LINK:-}"
    BUILD=$WORK/src/jk2-build
    OUT=${JK2_OUT:-$REPO/build/jk2/out}
fi
SRC=${JK2_SRC:-$WORK/src/openjk}
mkdir -p "$WORK/src" "$BUILD"

if [ ! -d "$SRC/.git" ]; then
    step "OpenJK ${COMMIT:0:8}"
    git clone -q https://github.com/JACoders/OpenJK.git "$SRC"
fi
# the tree is OpenJK's commit plus our patch, put there again on every build; only files whose text changes are
# written, so that a header the patch touches does not build everything again (OpenTTD's way)
put() { cmp -s "$1" "$2" || cp "$1" "$2"; }
if [ "${JK2_TREE:-}" != keep ]; then
    git -C "$SRC" cat-file -e "$COMMIT^{commit}" 2>/dev/null || git -C "$SRC" fetch -q origin
    P=$(mktemp -d); OLD=$SRC/.shaderemu-files
    : > "$P.files"
    if [ -s "$HERE/jk2.patch" ]; then
        git -C "$SRC" apply --numstat "$HERE/jk2.patch" | cut -f3 > "$P.files"
        # (a file the patch adds is not in the commit: git apply makes it)
        while read -r f; do mkdir -p "$P/$(dirname "$f")"; git -C "$SRC" show "$COMMIT:$f" > "$P/$f" 2>/dev/null || rm -f "$P/$f"; done < "$P.files"
        (cd "$P" && git apply "$HERE/jk2.patch")
        while read -r f; do put "$P/$f" "$SRC/$f"; done < "$P.files"
    fi
    for f in $( [ -f "$OLD" ] && grep -vxFf "$P.files" "$OLD" ); do git -C "$SRC" checkout -q "$COMMIT" -- "$f"; done
    mv "$P.files" "$OLD"; rm -rf "$P"
fi
S=$SRC

# q_version.h, which CMake would make: one date, so that the same sources are the same program
mkdir -p "$BUILD/shared/qcommon"
printf '#define SOURCE_DATE "2026-11-02"\n#define GIT_TAG "%s-shaderemu"\n' "${COMMIT:0:8}" > "$BUILD/shared/qcommon/q_version.h.new"
put "$BUILD/shared/qcommon/q_version.h.new" "$BUILD/shared/qcommon/q_version.h"; rm "$BUILD/shared/qcommon/q_version.h.new"
# GL's declarations, for the renderer: the Mesa header SDL carries (MIT), without SDL's own configuration; the GL calls
# themselves are nullgl.c's (no picture) or, later, the machine's
mkdir -p "$BUILD/include/GL"
printf '#pragma once\n#define SDL_config_h_\n#define GL_GLEXT_LEGACY\n#include "%s/lib/SDL2/include/SDL_opengl.h"\n' "$S" > "$BUILD/include/GL/gl.h.new"
put "$BUILD/include/GL/gl.h.new" "$BUILD/include/GL/gl.h"; rm "$BUILD/include/GL/gl.h.new"
[ -f "$BUILD/include/GL/glx.h" ] || printf '#pragma once\n' > "$BUILD/include/GL/glx.h"    # (qgl.h asks for it on Linux, and uses none of it)

printf '#define ARCH_STRING "%s"\n' "$ARCH" > "$BUILD/shared/se_arch.h.new"; put "$BUILD/shared/se_arch.h.new" "$BUILD/shared/se_arch.h"; rm "$BUILD/shared/se_arch.h.new"
COMMON="-include $BUILD/shared/se_arch.h -DFINAL_BUILD -DSHADEREMU -DNDEBUG -fsigned-char -w -I$BUILD/shared -I$S/shared -I$S/lib/gsl-lite/include"
CXXSTD="-std=c++14"

ENGINE="client/cl_cgame client/cl_cin client/cl_console client/cl_input client/cl_keys client/cl_main client/cl_mp3
    client/cl_parse client/cl_scrn client/cl_ui client/snd_ambient client/snd_dma client/snd_mem client/snd_mix client/snd_music
    client/vmachine qcommon/cm_load qcommon/cm_patch qcommon/cm_polylib qcommon/cm_test qcommon/cm_trace qcommon/cmd
    qcommon/common qcommon/cvar qcommon/files qcommon/md4 qcommon/msg qcommon/net_chan qcommon/q_shared
    qcommon/stringed_ingame qcommon/stringed_interface qcommon/strip qcommon/persistence qcommon/z_memman_pc
    qcommon/ojk_saved_game server/exe_headers server/sv_ccmds server/sv_client server/sv_game server/sv_init server/sv_main
    server/sv_savegame server/sv_snapshot server/sv_world game/genericparser2 ui/ui_atoms ui/ui_connect ui/ui_main
    ui/ui_saber ui/ui_shared ui/ui_syscalls"
MP3="cdct csbt csbtb csbtl3 cup cupini cupl1 cupl3 cwin cwinb cwinm hwin l3dq l3init mdct mhead msis towave uph upsf wavep"
SHARED_C="qcommon/q_color qcommon/q_math qcommon/q_string"
SHARED_CXX="qcommon/safe/files qcommon/safe/string"
ZLIB="adler32 compress crc32 deflate inffast inflate inftrees trees uncompr zutil"
GAME="AI_Atst AI_Default AI_Droid AI_GalakMech AI_Grenadier AI_Howler AI_ImperialProbe AI_Interrogator AI_Jedi AI_Mark1
    AI_Mark2 AI_MineMonster AI_Remote AI_Seeker AI_Sentry AI_Sniper AI_Stormtrooper AI_Utils G_Timer NPC NPC_behavior
    NPC_combat NPC_goal NPC_misc NPC_move NPC_reactions NPC_senses NPC_sounds NPC_spawn NPC_stats NPC_utils Q3_Interface
    Q3_Registers bg_misc bg_pangles bg_panimate bg_pmove bg_slidemove g_ICARUS g_active g_breakable g_camera g_client
    g_cmds g_combat g_functions g_fx g_inventory g_itemLoad g_items g_main g_mem g_misc g_misc_model g_missile g_mover
    g_nav g_navnew g_navigator g_object g_objectives g_ref g_roff g_savegame g_session g_spawn g_svcmds g_target
    g_trigger g_turret g_usable g_utils g_weapon g_weaponLoad genericparser2 wp_atst wp_blaster_rifle wp_bot_laser
    wp_bowcaster wp_bryar_pistol wp_demp2 wp_det_pack wp_disruptor wp_emplaced_gun wp_flechette wp_melee wp_repeater
    wp_rocket_launcher wp_saber wp_stun_baton wp_thermal wp_trip_mine"
CGAME="FX_ATSTMain FX_Blaster FX_Bowcaster FX_BryarPistol FX_DEMP2 FX_Disruptor FX_Emplaced FX_Flechette FX_HeavyRepeater
    FX_RocketLauncher FxPrimitives FxScheduler FxSystem FxTemplate FxUtil cg_camera cg_consolecmds cg_credits cg_draw
    cg_drawtools cg_effects cg_ents cg_event cg_info cg_lights cg_localents cg_main cg_marks cg_players cg_playerstate
    cg_predict cg_scoreboard cg_servercmds cg_snapshot cg_syscalls cg_text cg_view cg_weapons"
ICARUS="BlockStream Sequence Sequencer TaskManager Instance"
RENDERER="rd-vanilla/G2_API rd-vanilla/G2_bolts rd-vanilla/G2_bones rd-vanilla/G2_misc rd-vanilla/G2_surfaces qcommon/matcomp
    qcommon/q_shared rd-vanilla/tr_arb rd-vanilla/tr_backend rd-vanilla/tr_bsp rd-vanilla/tr_cmds rd-vanilla/tr_curve
    rd-vanilla/tr_draw rd-vanilla/tr_ghoul2 rd-vanilla/tr_image rd-vanilla/tr_init rd-vanilla/tr_light rd-vanilla/tr_main
    rd-vanilla/tr_marks rd-vanilla/tr_mesh rd-vanilla/tr_model rd-vanilla/tr_quicksprite rd-vanilla/tr_scene
    rd-vanilla/tr_shade rd-vanilla/tr_shade_calc rd-vanilla/tr_shader rd-vanilla/tr_shadows rd-vanilla/tr_skin
    rd-vanilla/tr_sky rd-vanilla/tr_stl rd-vanilla/tr_subs rd-vanilla/tr_surface rd-vanilla/tr_surfacesprites
    rd-vanilla/tr_world rd-vanilla/tr_WorldEffects rd-common/tr_font rd-common/tr_image_load rd-common/tr_image_jpg
    rd-common/tr_image_tga rd-common/tr_image_png rd-common/tr_noise"
JPEG="jaricom jcapimin jcapistd jcarith jccoefct jccolor jcdctmgr jchuff jcinit jcmainct jcmarker jcmaster jcomapi jcparam
    jcprepct jcsample jctrans jdapimin jdapistd jdarith jdatadst jdatasrc jdcoefct jdcolor jddctmgr jdhuff jdinput
    jdmainct jdmarker jdmaster jdmerge jdpostct jdsample jdtrans jerror jfdctflt jfdctfst jfdctint jidctflt jidctfst
    jidctint jmemmgr jmemnobs jquant1 jquant2 jutils"
PNG="png pngerror pngget pngmem pngpread pngread pngrio pngrtran pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil"

# Each part's objects: a list of "source object flags-name" lines, compiled JOBS at once, again only when the source, the
# patch, this script or a file of ours is newer than the object, or the part's flags changed
export CC CXX
compile() {   # part, then lines "source object compiler flags..."
    local part=$1; shift
    mkdir -p "$BUILD/$part"
    local flags_file="$BUILD/$part.flags"
    printf '%s\n' "$@" | md5sum > "$flags_file.new"
    cmp -s "$flags_file.new" "$flags_file" || rm -f "$BUILD/$part"/*.o
    mv "$flags_file.new" "$flags_file"
    printf '%s\n' "$@" | xargs -P "$JOBS" -I{} sh -c '
        set -- {}; src=$1; obj=$2; shift 2
        if [ "$obj" -nt "$src" ] && [ "$obj" -nt "'"$HERE"'/build.sh" ] && [ "$obj" -nt "'"$HERE"'/jk2.patch" ] &&
           [ "$obj" -nt "'"$HERE"'/sys_shaderemu.cpp" ] && [ "$obj" -nt "'"$HERE"'/nullgl.c" ] && [ "$obj" -nt "'"$HERE"'/se_rand.c" ] && [ "$obj" -nt "'"$HERE"'/cg_rand.c" ]; then exit 0; fi
        "$@" -c "$src" -o "$obj" || { echo "failed: $src"; exit 255; }'
}
lines() {   # part, compiler and flags, then sources (with their extension)
    local part=$1 cc=$2; shift 2
    for f in "$@"; do b=$(basename "$(dirname "$f")")_$(basename "$f"); echo "$f $BUILD/$part/${b%.*}.o $cc"; done   # (folder_file.o: files.cpp is in two folders)
}

# (sys_shaderemu.cpp is the engine's on the build computer; on the machine the window part's, with the window)
SYS_ENGINE="$HERE/sys_shaderemu.cpp"; [ "${JK2_HOST:-}" = 1 ] || { SYS_ENGINE=; rm -f "$BUILD/engine/jk2_sys_shaderemu.o"; }
EF="$OPT $COMMON -D_JK2EXE -DJK2_MODE -I$S/code -I$S/lib/minizip/include -I$S/lib/minizip/include/minizip -I$S/lib/zlib/include"
step "the engine"
compile engine \
    "$(lines engine "$CXX $CXXSTD $EF" $(for f in $ENGINE; do echo "$S/code/$f.cpp"; done) \
        $(for f in $SHARED_CXX; do echo "$S/shared/$f.cpp"; done) \
        "$S/shared/sys/sys_main.cpp" "$S/shared/sys/sys_event.cpp" "$S/shared/sys/con_log.cpp" "$S/shared/sys/sys_unix.cpp" \
        "$S/shared/sys/con_tty.cpp" $SYS_ENGINE)" \
    "$(lines engine "$CC $EF" $(for f in $MP3; do echo "$S/code/mp3code/$f.c"; done) \
        $(for f in $SHARED_C; do echo "$S/shared/$f.c"; done) "$S/lib/minizip/ioapi.c" "$S/lib/minizip/unzip.c" \
        $(for f in $ZLIB; do echo "$S/lib/zlib/$f.c"; done))"

# (-fno-gnu-unique: a static inside an inline function is otherwise a name objcopy cannot make the module's own)
GF="$OPT $COMMON -fno-gnu-unique -DJK2_MODE -DSP_GAME -I$S/codeJK2 -I$S/codeJK2/game"
step "the game"
compile game \
    "$(lines game "$CXX $CXXSTD $GF" $(for f in $GAME; do echo "$S/codeJK2/game/$f.cpp"; done) \
        $(for f in $ICARUS; do echo "$S/codeJK2/icarus/$f.cpp"; done) \
        "$S/code/ui/gameinfo.cpp" "$S/code/qcommon/tri_coll_test.cpp" "$S/code/qcommon/q_shared.cpp" "$S/code/Rufl/hstring.cpp" \
        $(for f in $SHARED_CXX; do echo "$S/shared/$f.cpp"; done))" \
    "$(lines game "$CC $GF" $(for f in $SHARED_C; do echo "$S/shared/$f.c"; done) "$HERE/se_rand.c" "$HERE/cg_rand.c")" \
    "$(lines game "$CXX $CXXSTD $GF -DSE_CG_UNIT" $(for f in $CGAME; do echo "$S/codeJK2/cgame/$f.cpp"; done))"
# (SE_CG_UNIT: the client's game's files, which read the game's entities through se_cg_gents, g_shared.h: the copy the
# game gives it, docs/games/jk2.md, "The game's frame apart")
# the client's game draws its random numbers from states of its own (cg_rand.c says why): its objects' calls renamed
for o in "$BUILD"/game/cgame_*.o; do
    $OBJCOPY --redefine-sym Q_flrand=CG_flrand --redefine-sym Q_irand=CG_irand --redefine-sym rand=CG_rand "$o"
done
# (q_shared.cpp of code/ and of the game would make one object name: the game's is in a folder of its own)

# the GL the renderer calls: nullgl.c's nothing on the build computer; on the machine gl_jk2.c's (the GPU once a window
# is open, nullgl.c's answers before), its calls renamed jk2_glX in the module below
NULLGL="$HERE/nullgl.c"; [ "${JK2_HOST:-}" = 1 ] || NULLGL=
RF="$OPT $COMMON -fno-gnu-unique -DRENDERER -D_JK2EXE -DJK2_MODE -I$S/code -I$S/code/rd-vanilla -I$S/code/rd-common -I$BUILD/include -I$S/lib/jpeg-9a/include -I$S/lib/jpeg-9a -I$S/lib/libpng/include -I$S/lib/zlib/include"
step "the renderer"
compile renderer \
    "$(lines renderer "$CXX $CXXSTD $RF" $(for f in $RENDERER; do echo "$S/code/$f.cpp"; done))" \
    "$(lines renderer "$CC $RF" $(for f in $SHARED_C; do echo "$S/shared/$f.c"; done) \
        $(for f in $JPEG; do echo "$S/lib/jpeg-9a/$f.c"; done) $(for f in $PNG; do echo "$S/lib/libpng/$f.c"; done) $NULLGL)"

# The game and the renderer were libraries with names of their own (q_shared's, Com_Printf, their globals): each is linked
# into one object whose names stay inside it, but for its one entry, renamed (sys_shaderemu.cpp finds them). Section
# groups (C++'s inline functions and templates) are made plain sections first, or the program's link would keep one
# module's copy and drop the other's that its names point into.
# The game's variables are two sections of their own, jk2game_data and jk2game_bss, and its constructors a third,
# jk2game_init, out of the program's: sys_shaderemu.cpp makes them fresh at each load as a library's were (the data as
# it began, the rest zero, the constructors run): a saved game's load found a table of the last load's freed names
# (G_LoadRoff, AddressSanitizer, 2 November 2026).
# On the machine every variable of its own section starts a 16-byte texel (OpenTTD's ottd-align16): the drawing's worker
# (gl_jk2.c) and the music's write statics beside this core's, and two cores must not store to one texel in one pass
if [ "${JK2_HOST:-}" != 1 ]; then
    for o in "$BUILD"/engine/*.o "$BUILD"/game/*.o "$BUILD"/renderer/*.o; do
        $OBJCOPY --set-section-alignment '.data*=16' --set-section-alignment '.sdata*=16' --set-section-alignment '.bss*=16' \
            --set-section-alignment '.sbss*=16' "$o"
    done
fi
step "the modules"
cat > "$BUILD/game.ld.new" <<'EOF'
SECTIONS {
    jk2game_data : { *(.data .data.* .sdata .sdata.*) }
    jk2game_bss : { *(.bss .bss.* .sbss .sbss.* COMMON) }
    jk2game_init : { *(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*) .init_array .ctors) }
}
EOF
put "$BUILD/game.ld.new" "$BUILD/game.ld"; rm "$BUILD/game.ld.new"
module() {   # part, entries
    local part=$1; shift
    local script=; [ "$part" = game ] && script="-T $BUILD/game.ld"
    $LD -r --force-group-allocation $script -o "$BUILD/$part.all.o" "$BUILD/$part"/*.o
    $OBJCOPY $(for e in "$@"; do echo "--keep-global-symbol=$e"; done) "$BUILD/$part.all.o" "$BUILD/$part.o"
    $OBJCOPY $(for e in "$@"; do echo "--redefine-sym $e=se_$e"; done) "$BUILD/$part.o"
}
module game GetGameAPI dllEntry vmMain
module renderer GetRefAPI
if [ "${JK2_HOST:-}" != 1 ]; then
    $OBJCOPY $($NM -u "$BUILD/renderer.o" | awk '$2 ~ /^gl[A-Z]/ {print "--redefine-sym " $2 "=jk2_" $2}') "$BUILD/renderer.o"
fi

MACHINE=
if [ "${JK2_HOST:-}" != 1 ]; then
    # The window, the GPU and the workers' library (Half-Life's GL over Quake III's draws on gles.c): docs/games/jk2.md, "The picture"
    step "the window"
    PROGRAMS=$REPO/programs
    MW=${SE_MW:-$WORK/src/microwindows}
    [ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }
    WF="$OPT -fno-pie -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable"
    compile window \
        "$(lines window "$CXX $CXXSTD $EF -DSE_WINDOW -I$MW/src/include -I$PROGRAMS/mc -I$REPO/linux/libmedia" "$HERE/sys_shaderemu.cpp")" \
        "$(lines window "$CC $WF -DSEGL_FLOAT -DMAX_TEXTURES=64 -idirafter $PROGRAMS/linux/include -I$MW/src/include -I$PROGRAMS/linux -I$REPO/linux/quake3" "$REPO/linux/quake3/gpu_shaderemu.c")" \
        "$(lines window "$CC $WF -I$REPO/linux/quake3 -I$PROGRAMS/mc" "$HERE/gl_jk2.c")" \
        "$(lines window "$CC -O2 -fno-common -fdata-sections -w -I$PROGRAMS/mc" "$PROGRAMS/mc/mcw.c" "$PROGRAMS/mc/mcwx.c")"
    # libmedia (docs/system/media.md): the music decoded on the worker cores (sys_shaderemu.cpp, "Music on the card")
    MEDIA=${MEDIA_LIB:-$WORK/src/libmedia/libmedia.a}
    [ -f "$MEDIA" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }
    for o in "$BUILD"/window/*.o; do   # (a texel each, as above)
        $OBJCOPY --set-section-alignment '.data*=16' --set-section-alignment '.sdata*=16' --set-section-alignment '.bss*=16' \
            --set-section-alignment '.sbss*=16' "$o"
    done
    # (every strtod and atof through sys_shaderemu.cpp's __wrap_strtod and __wrap_atof: musl's parses in software long doubles)
    MACHINE="$BUILD/window/*.o $MW/src/lib/libnano-X.a $MEDIA -Wl,--wrap=strtod -Wl,--wrap=atof $SE_WRAP"
fi

step "the program"
$CXX $OPT -o "$BUILD/jk2" "$BUILD/engine"/*.o "$BUILD/game.o" "$BUILD/renderer.o" $MACHINE $LIBS
$NM -n -C "$BUILD/jk2" > "$BUILD/jk2.nm"
if [ "${JK2_HOST:-}" = 1 ]; then
    cp "$BUILD/jk2" "$OUT/jk2"
    ls -l "$OUT/jk2"
else
    mkdir -p "$OUT/usr/bin"
    "$CXXTC/bin/$T-strip" -o "$OUT/usr/bin/jk2.bin" "$BUILD/jk2"
    "$NM" -n -C "$BUILD/jk2" > "$WORK/src/jk2.nm"
    # the starter: the demo's installer where data.py puts it (/usr/share/jk2), unpacked into memory once a session after
    # its licence is accepted; what the game writes where the host keeps it
    cat > "$OUT/usr/bin/jk2" <<'EOF'
#!/bin/sh
# Jedi Outcast's demo (docs/games/jk2.md): its pak lies decoded in the ROM and is linked into /tmp/jk2 at each start,
# after its licence is accepted (cabinst, docs/system/install.md); the links are deleted when the game ends
# (the image has the installer decoded since 4 November 2026, linux/jk2/data.py: the licence shown, the pak linked into place)
if [ -d /usr/share/jk2/installed ]; then
    cabinst --title "Jedi Outcast (demo)" --licence Disk1/Install/license.txt --key jk2.eula --installed /usr/share/jk2/installed /tmp/jk2 || exit $?
else
    cabinst --title "Jedi Outcast (demo)" --licence Disk1/Install/license.txt --key jk2.eula /usr/share/jk2/jk2demo.exe /tmp/jk2 \
    Disk1/GameData/demo/assets0.pk3=base/assets0.pk3 || exit $?
fi
[ "$1" = --unpack ] && exit 0	# (only that: a test unpacks first, then plays)
# settings and saved games in the folder the host keeps, else in /tmp
P=/persistence; mkdir -p $P/jk2 2>/dev/null || P=/tmp
# (Ctrl fires as in the other shooters and the world's controller: the demo binds it to use, which E is too; the
# quotes keep the engine's command line from cutting the bind at its +); pictures a quarter of their size each way, the
# GPU's memory being 36 MB of RAM (gl_jk2.c); triangles as glDrawElements, not strips of glArrayElement (four thousand
# primitives a frame, each a draw of its own)
# (not exec: the shell waits, and deletes the pak however the game ends)
trap 'rm -rf /tmp/jk2' EXIT; trap 'exit 143' TERM INT HUP
jk2.bin +set fs_basepath /tmp/jk2 +set fs_homepath $P/jk2 +set com_demo 1 +bind CTRL '"+attack"' +set r_picmip 2 +set r_primitives 2 "$@"
EOF
    chmod 755 "$OUT/usr/bin/jk2"
    # the Start menu's entry; data.py puts it into an image with the demo, never into the shared one
    mkdir -p "$OUT/usr/share"
    echo "Games/Jedi Outcast (demo)=jk2" > "$OUT/usr/share/nxapps.77-jk2"
    ls -l "$OUT/usr/bin/jk2.bin"
    # JK2_SKINTEST=1: skintest, the GPU's skinned draws against this core's (skintest.c; a test, not in the image)
    if [ "${JK2_SKINTEST:-}" = 1 ]; then
        step "skintest"
        $CC $WF -ffp-contract=off -DSEGL_FLOAT -idirafter $PROGRAMS/linux/include -I$MW/src/include -I$PROGRAMS/linux -I$REPO/linux/quake3 \
            -o "$BUILD/skintest" "$HERE/skintest.c" "$BUILD/window/quake3_gpu_shaderemu.o" "$BUILD/window/mc_mcw.o" \
            $MW/src/lib/libnano-X.a -lm
        cp "$BUILD/skintest" "$OUT/usr/bin/skintest"
    fi
fi
step done
