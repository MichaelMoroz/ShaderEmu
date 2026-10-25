#!/usr/bin/env bash
# Builds Half-Life: Uplink's engine on Xash3D FWGS for the Linux image (docs/games/halflife.md): the
# engine, its renderer, Half-Life's client and server (hlsdk-portable) linked into one program,
# with a window, an OpenGL and input of our own. Uplink's data is not in it: data.py fetches it
# when an image is made, and it never goes into linux/prebuilt.
#   wsl -- bash linux/nommu/build.sh halflife/build.sh        (after nanox and libmedia)
# In the guest: halflife (the splash, the opening film, the main menu), or halflife +map hldemo1
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
MW=${SE_MW:-$WORK/src/microwindows}
X=${HALFLIFE_XASH:-$WORK/src/xash3d}
H=${HALFLIFE_HLSDK:-$WORK/src/hlsdk}
BUILD=${HALFLIFE_BUILD:-$WORK/src/halflife-build}
OUT=${HALFLIFE_OUT:-$REPO/build/images/linux/root}
PROGRAMS=${SE_PROGRAMS:-$REPO/programs}
XASH_COMMIT=00d9514c
HLSDK_COMMIT=6c168fc7
[ -f "$MW/src/lib/libnano-X.a" ] || { echo "run linux/nanox/build.sh first"; exit 1; }

# the trees: each a commit plus our patch, put there again unless it is that already
# (HALFLIFE_TREE=keep: the trees as they stand, for writing the patches)
tree() {   # url, folder, commit, patch
    if [ ! -d "$2/.git" ]; then
        echo "== $(basename "$2") $3"
        git clone -q "$1" "$2"
    fi
    if [ "${HALFLIFE_TREE:-}" != keep ] && ! { [ "$(git -C "$2" rev-parse HEAD)" = "$(git -C "$2" rev-parse "$3^{commit}")" ] &&
            git -C "$2" diff | cmp -s - "$4"; }; then
        git -C "$2" checkout -q -f "$3" && git -C "$2" clean -q -fd -e 3rdparty
        [ ! -s "$4" ] || git -C "$2" apply "$4"
    fi
}
[ ! -d "$X/.git" ] || git -C "$X" config submodule.3rdparty/mainui.ignore dirty   # (mainui.patch is its own)
tree https://github.com/FWGS/xash3d-fwgs.git "$X" $XASH_COMMIT "$HERE/xash3d.patch"
tree https://github.com/FWGS/hlsdk-portable.git "$H" $HLSDK_COMMIT "$HERE/hlsdk.patch"
git -C "$X" submodule update -q --init 3rdparty/extras 3rdparty/library_suffix 3rdparty/MultiEmulator 3rdparty/bzip2/bzip2
git -C "$X" config submodule.3rdparty/mainui.ignore dirty
git -C "$X" submodule update -q --init --recursive 3rdparty/mainui
# the menu as Uplink's (its readme, Order Half-Life, no multiplayer): mainui.patch, written in 3rdparty/mainui
# by git add -A -N; git diff (new files too)
MT=$X/3rdparty/mainui
git -C "$MT" add -A -N
if [ "${HALFLIFE_TREE:-}" != keep ] && ! git -C "$MT" diff | cmp -s - "$HERE/mainui.patch"; then
    git -C "$MT" reset -q && git -C "$MT" checkout -q -f . && git -C "$MT" clean -q -fd
    git -C "$MT" apply "$HERE/mainui.patch" && git -C "$MT" add -A -N
fi
git -C "$H" submodule update -q --init freevgui

FPU="-fno-math-errno -fsingle-precision-constant"
OPT="-O2 -fno-tree-vectorize"
HAVE="-D_GNU_SOURCE -DALLOCA_H=<alloca.h> -DHAVE_TGMATH_H=1 -DHAVE_STRNCASECMP=1 -DHAVE_STRCASECMP=1 -DHAVE_STRCASESTR=1
    -DHAVE_STRCHRNUL=1 -DHAVE_STRLCPY=1 -DHAVE_STRLCAT=1 -DHAVE_STRNLEN=1 -DHAVE_DIRENT_D_TYPE=1"
XDEFS="-DXASH_SHADEREMU=1 -DXASH_STATIC_LIBS=1 -DXASH_GAMEDIR=\"valve\" -DXASH_LOW_MEMORY=1 -DXASH_MINIZ=1
    -DMINIZ_NO_ARCHIVE_APIS -DXASH_REF_GL_ENABLED=1 -DXASH_BUILD_COMMIT=\"$XASH_COMMIT\" -DXASH_BUILD_BRANCH=\"shaderemu\" -DXASH_BUILD_COMMIT_DATE=\"2026-10-16\" $HAVE"
XINC="-I$X/engine -I$X/common -I$X/public -I$X/pm_shared -I$X/filesystem -I$X/3rdparty/miniz -I$X/3rdparty/library_suffix/include"
EINC="-I$X/engine -I$X/engine/common -I$X/engine/common/imagelib -I$X/engine/common/soundlib -I$X/engine/server
    -I$X/engine/client -I$X/engine/client/vgui -I$X/engine/platform -I$X/3rdparty/MultiEmulator/include -I$X/3rdparty/MultiEmulator/src -I$X/3rdparty/bzip2/bzip2 -I$MW/src/include"
C_ALL="$OPT $FPU -fno-pie -fno-common -fdata-sections -fsigned-char -fno-strict-aliasing -w"
HLDEFS="-DXASH_SHADEREMU=1 -DXASH_BUILD_COMMIT=\"$HLSDK_COMMIT\" -DHAVE_TGMATH_H=1 -DHAVE_CMATH=1 -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp -D_snprintf=snprintf
    -D_vsnprintf=vsnprintf -D_LINUX=1 -DLINUX=1 -DCLIENT_WEAPONS=1 -DCROWBAR_FIX_RAPID_CROWBAR=1 -DSPEAKABLE_TARGETS=1
    -DHAVE_STRLCPY=1 -DHAVE_STRLCAT=1"
HLINC="-I$H/dlls -I$H/common -I$H/engine -I$H/pm_shared -I$H/game_shared -I$H/public"
# A variable a section (-fdata-sections), each starting on 16 bytes: the server's and the drawing's workers store
# beside core 0, and two cores must not store to one 16-byte texel in a pass (gpu_vm's whole-texel commit, GVM_RACES)
ALIGN16="--set-section-alignment .data*=16 --set-section-alignment .sdata*=16 --set-section-alignment .bss*=16 --set-section-alignment .sbss*=16"
export HERE BUILD ALIGN16
mkdir -p "$BUILD" && cd "$BUILD"

# one object a source (its path made a name), made again when the source, a patch or this script is newer
compile() {   # folder, compiler and flags (one word, quoted), then sources
    local dir=$1 cc=$2; shift 2
    mkdir -p "$BUILD/$dir"
    printf '%s\n' "$@" | CC="$cc" DIR="$dir" xargs -P "$(nproc)" -I{} sh -c '
        o="$BUILD/$DIR/$(echo {} | sed "s|^/||; s|/|_|g; s|\.[a-z]*$||").o"
        [ "$o" -nt {} ] && [ "$o" -nt "$HERE/xash3d.patch" ] && [ "$o" -nt "$HERE/hlsdk.patch" ] && [ "$o" -nt "$HERE/build.sh" ] && [ "$o" -nt "$HERE/gl_kept.c" ] && [ "$o" -nt "$HERE/gl_studio_job.c" ] && [ "$o" -nt "$HERE/se_server.h" ] ||
            { $CC -c {} -o "$o" && riscv32-linux-objcopy $ALIGN16 "$o" || { echo "failed: {}"; exit 255; }; }'
}
# A library as Xash's static build makes one: its objects as one, every name inside it local but
# lib_NAME_exports, a table of the names the engine asks it for (generated_library_tables.h).
library() {   # name, names file, folders of objects
    local name=$1 names=$2; shift 2
    riscv32-linux-ld -r -o "$name.pre.o" $(for d in "$@"; do ls "$BUILD/$d"/*.o; done)
    { while read -r f; do echo "extern void $f(void);"; done < "$names"
      echo "struct { const char *name; void *func; } lib_${name}_exports[] = {"
      while read -r f; do echo "{ \"$f\", (void *)&$f },"; done < "$names"
      echo "{ 0, 0 } };"; } > "$name.table.c"
    rv32-cc -O2 -fno-pie -c "$name.table.c" -o "$name.table.o"
    riscv32-linux-ld -r -o "$name.all.o" "$name.pre.o" "$name.table.o"
    # (no COMDAT groups: the two game libraries share inline functions, each its own once local)
    riscv32-linux-objcopy -R .group -G "lib_${name}_exports" "$name.all.o" "$name.o"
}
# every function of a game library with a C name: what the engine looks up (an entity's class too)
c_names() { riscv32-linux-nm -g --defined-only "$1" | awk '$2 == "T" && $3 !~ /^_Z/ { print $3 }' | sort -u; }

PUBLIC="$(ls $X/public/*.c | grep -v 'getopt\|build_vcs') $X/3rdparty/library_suffix/src/library_suffix.c"
# the filesystem
compile fs "rv32-cc $C_ALL $XDEFS $XINC -idirafter $REPO/linux/libmedia -idirafter $PROGRAMS/mc" $X/filesystem/*.c $PUBLIC
compile fs "rv32-c++ $C_ALL -std=gnu++11 $XDEFS $XINC" $X/filesystem/VFileSystem009.cpp
printf 'GetFSAPI\nCreateInterface\n' > fs.names
library filesystem_stdio fs.names fs

# the renderer, ref/gl, on gl_shaderemu.c
compile ref "rv32-cc $C_ALL $XDEFS -DREF_DLL=1 -I$X/ref/gl -I$HERE -I$X/ref/common $XINC -I$X/engine/common -I$X/engine/common/imagelib" $X/ref/gl/*.c $X/ref/gl/gl2_shim/*.c $X/ref/common/*.c $PUBLIC
echo GetRefAPI > ref.names
library ref_gl ref.names ref

# the engine: everything but what the image has no use for (MP3, Ogg, voice, the Steam ticket)
ENGINE=$(ls $X/engine/common/*.c $X/engine/common/imagelib/*.c $X/engine/common/soundlib/*.c $X/engine/server/*.c \
    $X/engine/common/http/*.c $X/engine/platform/posix/*.c $X/engine/platform/linux/sys_linux.c $X/engine/platform/misc/lib_static.c \
    $(find $X/engine/client -name '*.c') $X/3rdparty/miniz/miniz.c $X/3rdparty/MultiEmulator/src/*.c $PUBLIC $X/public/build_vcs.c |
    grep -v 'snd_mp3\|snd_ogg\|ogg_filestream\|sound/voice\.c\|crash_libbacktrace\|avi_ffmpeg')
rm -f "$BUILD"/engine/*_engine_platform_stub_s_stub.o "$BUILD"/engine/*_avi_avi_ffmpeg.o   # (what builds before snd_shaderemu.c and avi_shaderemu.c had)
# the table the engine finds the libraries in
cat > generated_library_tables.h <<'EOF'
extern table_t lib_filesystem_stdio_exports[];
extern table_t lib_ref_gl_exports[];
extern table_t lib_client_exports[];
extern table_t lib_server_exports[];
extern table_t lib_menu_exports[];
struct {const char *name;void *func;} libs[] = {
{ "filesystem_stdio", &lib_filesystem_stdio_exports },
{ "ref_gl", &lib_ref_gl_exports },
{ "client", &lib_client_exports },
{ "server", &lib_server_exports },
{ "menu", &lib_menu_exports },
{0,0}
};
EOF
# (lib_static.c includes it: made again when the table is newer)
[ "$BUILD/engine/$(echo $X/engine/platform/misc/lib_static.c | sed 's|^/||; s|/|_|g; s|\.c$||').o" -nt generated_library_tables.h ] ||
    rm -f "$BUILD/engine/$(echo $X/engine/platform/misc/lib_static.c | sed 's|^/||; s|/|_|g; s|\.c$||').o"

# bzip2, for the network's compressed fragments
sed 's/@BZ_VERSION@/1.1.0-fwgs/' "$X/3rdparty/bzip2/bzip2/bz_version.h.in" > bz_version.h
compile engine "rv32-cc $C_ALL -DBZ_NO_STDIO -DBZ_UNIX -I$BUILD" $(for f in blocksort huffman crctable randtable compress decompress bzlib; do echo $X/3rdparty/bzip2/bzip2/$f.c; done)
compile engine "rv32-cc $C_ALL $XDEFS -DENGINE_DLL=1 -DXASH_ENABLE_MAIN=1 -I$BUILD $XINC $EINC -I$X/engine/client/avi -idirafter $HERE" $ENGINE \
    $HERE/stubs_shaderemu.c $HERE/libm_shaderemu.c $HERE/avi_shaderemu.c
compile engine "rv32-cc $C_ALL $XDEFS -DENGINE_DLL=1 -I$BUILD $XINC $EINC -idirafter $HERE -idirafter $PROGRAMS/linux/include" $HERE/vid_shaderemu.c
compile engine "rv32-cc $C_ALL $XDEFS -DENGINE_DLL=1 -I$BUILD $XINC $EINC -idirafter $HERE -idirafter $REPO/linux/libmedia -idirafter $PROGRAMS/mc" $HERE/snd_shaderemu.c
compile engine "rv32-cc $C_ALL $XDEFS -DENGINE_DLL=1 -I$BUILD $XINC $EINC -idirafter $REPO/linux/libmedia" $HERE/snd_media.c $HERE/img_media.c
compile engine "rv32-cc $C_ALL $XDEFS -DENGINE_DLL=1 -I$BUILD $XINC $EINC -idirafter $HERE -idirafter $PROGRAMS/mc" $HERE/sv_shaderemu.c $HERE/rnd_shaderemu.c
# Half-Life's server
SERVER=$(find $H/dlls -name '*.cpp' | grep -v 'mpstubb\|stats\.cpp\|Wxdebug')
compile server "rv32-c++ $C_ALL -include $HERE/float_math.h -std=gnu++11 $HLDEFS $HLINC -I$H/dlls" $SERVER $H/game_shared/voice_gamemgr.cpp
compile server "rv32-cc $C_ALL $HLDEFS $HLINC" $H/pm_shared/*.c $H/public/safe_snprintf.c $H/game_shared/vcs_info.c
riscv32-linux-ld -r -o server.names.o "$BUILD"/server/*.o && c_names server.names.o > server.names
library server server.names server

# Half-Life's client, with freevgui inside it
CLIENT=$(find $H/cl_dll -name '*.cpp' | grep -v GameStudioModelRenderer_Sample)
SHARED=$(ls $H/game_shared/*.cpp | grep -v 'voice_vgui_tweakdlg\|voice_gamemgr\|voice_status')
WEAPONS=$(for w in crossbow crowbar egon gauss glock handgrenade hornetgun mp5 python rpg satchel shotgun squeakgrenade tripmine; do echo $H/dlls/$w.cpp; done)
VGUI=$(ls $H/freevgui/*.cpp $H/freevgui/controls/*.cpp $H/freevgui/platform/common/*.cpp $H/freevgui/platform/posix/*.cpp $H/freevgui/platform/xash3d-fwgs/*.cpp 2>/dev/null)
CLINC="-I$H/cl_dll -I$H/utils/vgui/include -I$H/freevgui -I$H/external"
compile client "rv32-c++ $C_ALL -include $HERE/float_math.h -frtti -std=gnu++11 $HLDEFS -DCLIENT_DLL -DINTERNAL_VGUI_SUPPORT $HLINC $CLINC" $CLIENT $SHARED $WEAPONS
compile client "rv32-cc $C_ALL $HLDEFS -DCLIENT_DLL $HLINC" $H/pm_shared/*.c $H/public/safe_snprintf.c $H/game_shared/vcs_info.c
compile vgui "rv32-c++ $C_ALL -include $HERE/float_math.h -frtti -std=gnu++11 -DINTERNAL_VGUI_SUPPORT -I$H/freevgui -I$H/engine -I$H/common -I$H/public" $VGUI
riscv32-linux-ld -r -o client.names.o "$BUILD"/client/*.o "$BUILD"/vgui/*.o && c_names client.names.o > client.names
library client client.names client vgui

# the main menu, mainui (its own bitmap font)
M=$X/3rdparty/mainui
MENU=$(find $M -maxdepth 1 -name "*.cpp"; for d in miniutl font menus menus/dynamic model controls; do find $M/$d -maxdepth 1 -name "*.cpp"; done)
compile menu "rv32-c++ $C_ALL -DXASH_SHADEREMU=1 -include $HERE/float_math.h -fno-rtti -std=gnu++11 -DSTDINT_H=<cstdint> -DMAINUI_USE_CUSTOM_FONT_RENDER=1 $(for d in . miniutl font controls menus model sdk_includes/common sdk_includes/engine sdk_includes/public sdk_includes/pm_shared; do echo -I$M/$d; done)" $MENU
printf 'GetMenuAPI\nGetExtAPI\n' > menu.names
library menu menu.names menu

# the GPU: gles.c with Quake III's draws, and the OpenGL on them
rv32-cc $OPT $FPU -fno-pie -fno-common -fdata-sections -Wall -c -DSEGL_FLOAT -DMAX_TEXTURES=64 -idirafter "$PROGRAMS/linux/include" \
    -I"$MW/src/include" -I"$PROGRAMS/linux" -I"$REPO/linux/quake3" "$REPO/linux/quake3/gpu_shaderemu.c" -o gpu.o
rv32-cc $OPT $FPU -fno-pie -fno-common -fdata-sections -Wall -c -I"$REPO/linux/quake3" -I"$PROGRAMS/mc" "$HERE/gl_shaderemu.c" -o gl.o
rv32-c++ -O2 -fno-pie -c "$HERE/cxxrt.cpp" -o cxxrt.o
# freevgui casts down: the client has run-time type information, and rtti_shaderemu.c its casts
rv32-cc -O2 -fno-pie -Wall -c "$HERE/rtti_shaderemu.c" -o rtti.o
rv32-cc $OPT $FPU -fno-pie -Wall -c "$HERE/fastmath.c" -o fastmath.o
MEDIA=$WORK/src/libmedia
[ -f "$MEDIA/libmedia.a" ] || { echo "run linux/libmedia/build.sh first"; exit 1; }
# the workers' library from this tree (another build's copy of it may be older)
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -w -I"$PROGRAMS/mc" -c "$PROGRAMS/mc/mcw.c" -o mcw.o
rv32-cc -O2 -fno-pie -fno-common -fdata-sections -w -I"$PROGRAMS/mc" -c "$PROGRAMS/mc/mcwx.c" -o mcwx.o
for o in gpu.o gl.o mcw.o mcwx.o; do riscv32-linux-objcopy $ALIGN16 "$o"; done
rv32-c++ -O2 $FPU "$BUILD"/engine/*.o filesystem_stdio.o ref_gl.o client.o server.o menu.o gpu.o gl.o cxxrt.o rtti.o fastmath.o \
    "$MEDIA/libmedia.a" mcw.o mcwx.o "$MW/src/lib/libnano-X.a" -lm -o halflife \
    -Wl,--wrap=malloc,--wrap=free,--wrap=calloc,--wrap=realloc,--wrap=posix_memalign,--wrap=aligned_alloc,--wrap=memalign
mkdir -p "$OUT/usr/bin"
riscv32-linux-strip -o "$OUT/usr/bin/halflife.bin" halflife
riscv32-linux-nm -n halflife > "${HALFLIFE_NM:-$WORK/src/halflife.nm}"
ls -l "$OUT/usr/bin/halflife.bin"

G=$OUT/usr/share/halflife/valve
mkdir -p "$G"
cp "$X/3rdparty/extras/xash-extras/"*.fnt "$G/" 2>/dev/null || true
mkdir -p "$G/resource" && cp "$HERE/gameui_english.txt" "$G/resource/"
cp "$H/LICENSE" "$OUT/usr/share/halflife/LICENSE-hlsdk"   # (its terms: a copy goes with every build)
# the menu's pictures Uplink lacks (checkboxes, arrows), a pk3dir after pak0 (data.py removes those pak0 has)
rm -rf "$G/extras.pk3dir" && mkdir -p "$G/extras.pk3dir/gfx/shell"
cp "$X/3rdparty/extras/xash-extras/gfx/shell/"* "$G/extras.pk3dir/gfx/shell/"
cat > "$G/gameinfo.txt" <<'EOF'
title "Half-Life"
gamedir "valve"
startmap "hldemo1"
type "singleplayer_only"
EOF
# the game wants its folder writable (its settings, saved games): the starter makes one of links
cat > "$OUT/usr/bin/halflife" <<'EOF'
#!/bin/sh
[ -f /usr/share/halflife/valve/pak0.PAK ] || { echo "halflife: Uplink's data is not in this image (linux/halflife/data.py)" >&2; exit 1; }
mkdir -p /tmp/halflife/valve /persistence/halflife && cd /tmp/halflife || exit 1
for f in /usr/share/halflife/valve/*; do ln -sf "$f" valve/; done
exec halflife.bin "$@"
EOF
chmod +x "$OUT/usr/bin/halflife"
echo "Games/Half-Life: Uplink=halflife" > "$OUT/usr/share/nxapps.72-halflife"
du -sh "$G"
