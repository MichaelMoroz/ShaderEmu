#!/usr/bin/env bash
# Builds libmedia (docs/system/media.md): pictures, sound and films on the worker cores, for every
# program of the image. Leaves $WORK/src/libmedia/libmedia.a and puts it in the image as
# /usr/lib/libmedia.a with /usr/include/media.h; a program links it with mcw.c and mcwx.c.
#   wsl -- bash /mnt/c/.../linux/libmedia/build.sh        (after linux/kernel/build.sh)
# Also builds media_test for this computer ($WORK/src/libmedia/media_test-host), whose sums the
# guest's must equal (test/check_pictures.py compares it with Pillow).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
source "$HERE/../userland/toolchain.sh"
BUILD=$WORK/src/libmedia
OUT=$REPO/build/images/linux/root/usr
mkdir -p "$BUILD/fetch" "$BUILD/obj" "$BUILD/host" "$OUT/lib" "$OUT/include" "$OUT/bin"

# The decoders of others are one header each, fetched at one commit (checked by their sums)
# and not kept in this repository; each says its licence at its top.
fetch() {
    [ -f "$BUILD/fetch/$1" ] || curl -sSfL "$2" -o "$BUILD/fetch/$1"
    echo "$3  $BUILD/fetch/$1" | sha256sum -c --quiet -
}
# ---- sound (m_audio*.c)
# minimp3 is CC0, dr_flac public domain or MIT No Attribution, stb_vorbis public domain or MIT
fetch minimp3.h https://raw.githubusercontent.com/lieff/minimp3/ea99364f61c14656440e8d77e9c233ccf3124633/minimp3.h \
    57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad
fetch dr_flac.h https://raw.githubusercontent.com/mackron/dr_libs/dfe8377631000664666519fdb83da193fd8037f4/dr_flac.h \
    111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c
fetch stb_vorbis.c https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_vorbis.c \
    4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636
# minimp3 with a half rate and frames that only fill the bit reservoir (minimp3.patch); a FLAC
# subframe in 32-bit sums whenever its coefficients allow (flac_bound.py)
cp "$BUILD/fetch/minimp3.h" "$BUILD/fetch/dr_flac.h" "$BUILD/fetch/stb_vorbis.c" "$BUILD/"
# a Vorbis block size's tables in floats, not sines in soft doubles (vorbis_tables.py)
python3 "$HERE/vorbis_tables.py" "$BUILD/stb_vorbis.c"
tr -d '\r' < "$HERE/minimp3.patch" | patch -s "$BUILD/minimp3.h"
python3 "$HERE/flac_bound.py" "$BUILD/dr_flac.h"
# ---- films (m_video*.c)
# pl_mpeg (Dominic Szablewski, MIT), changed as linux/video/build.sh had it (docs/apps/video.md): a
# stream's bytes are not moved down as they are read (they are every core's), the sound's
# window four blocks a turn, bits and codes from a word, the transform only where a block has
# coefficients. The same sums as before each change.
fetch pl_mpeg.h https://raw.githubusercontent.com/phoboslab/pl_mpeg/c871f2be022ece7ef4f64230b4fb8e1fb9eb6023/pl_mpeg.h \
    3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9
sed 's/^\t\tplm_buffer_discard_read_bytes(self->buffer);$/\t\tif (self->buffer->discard_read_bytes) plm_buffer_discard_read_bytes(self->buffer);/' \
    "$BUILD/fetch/pl_mpeg.h" > "$BUILD/pl_mpeg.h"
[ "$(grep -c 'if (self->buffer->discard_read_bytes) plm_buffer_discard_read_bytes' "$BUILD/pl_mpeg.h")" = 1 ] || { echo "pl_mpeg.h: the line to change is not there"; exit 1; }
python3 "$REPO/linux/video/plm_window.py" "$BUILD/pl_mpeg.h"
python3 "$REPO/linux/video/plm_fast.py" "$BUILD/pl_mpeg.h"

# What each part is built from; the decoders at -O3 with loops unrolled (docs/apps/play.md).
PICTURES="m_core m_simple m_png m_png_enc m_jpeg m_jpeg_enc m_gif m_gif_enc m_inflate"
AUDIO="m_audio m_audio_mod m_audio_mp3 m_audio_mp3dec m_audio_mp3half m_audio_flac m_audio_vorbis"
VIDEO="m_vq m_video m_video_mpeg m_video_vqa m_avi"
DECODE="-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops"
FLOAT="-fsingle-precision-constant -fno-math-errno"
INC="-I$HERE -I$BUILD -I$REPO/programs/mc"
rm -f "$BUILD/libmedia.a"
# (each file on a core of the build computer: one after another it was 31 s with the host's test)
pids=""
for f in $PICTURES $AUDIO $VIDEO; do
    rv32-cc $DECODE $FLOAT -Wall -Wno-unused-function -fdata-sections -ffunction-sections $INC -c "$HERE/$f.c" -o "$BUILD/obj/$f.o" &
    pids="$pids $!"
done
for p in $pids; do wait $p; done
riscv32-linux-ar rcs "$BUILD/libmedia.a" $(for f in $PICTURES $AUDIO $VIDEO; do echo "$BUILD/obj/$f.o"; done)
cp "$BUILD/libmedia.a" "$OUT/lib/libmedia.a"
cp "$HERE/media.h" "$OUT/include/media.h"

# media_test for the guest and for this computer
rv32-cc -O2 -w $INC -c "$REPO/programs/mc/mcw.c" -o "$BUILD/obj/mcw.o"
rv32-cc -O2 -w $INC -c "$REPO/programs/mc/mcwx.c" -o "$BUILD/obj/mcwx.o"
rv32-cc -O2 -w $INC "$HERE/test/media_test.c" "$BUILD/libmedia.a" "$BUILD/obj/mcw.o" "$BUILD/obj/mcwx.o" -lm -o "$BUILD/media_test"
riscv32-linux-strip -o "$OUT/bin/media_test" "$BUILD/media_test"
pids=""
for f in $PICTURES $AUDIO $VIDEO; do
    # (the sound's with the machine's single-precision constants: the same samples as the guest's)
    case " $AUDIO " in *" $f "*) F=$FLOAT;; *) F="";; esac
    gcc -O2 -w $F $INC -c "$HERE/$f.c" -o "$BUILD/host/$f.o" &
    pids="$pids $!"
done
for p in $pids; do wait $p; done
gcc -O2 -w $INC $(for f in $PICTURES $AUDIO $VIDEO; do echo "$BUILD/host/$f.o"; done) "$HERE/test/mcw_host.c" "$HERE/test/media_test.c" \
    -lpthread -lm -o "$BUILD/media_test-host"
ls -l "$OUT/lib/libmedia.a" "$OUT/bin/media_test"
