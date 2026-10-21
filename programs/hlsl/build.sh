#!/usr/bin/env bash
# libhlsl.a for the machine: the HLSL compiler and its drawing on the lanes (hlsl.h).
#   wsl -- bash programs/hlsl/build.sh [OUT_DIR]     (default build/hlsl)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$REPO/build/hlsl}
export PATH="$HOME/nm/work/bin:$HOME/shaderemu-linux/bin:$HOME/shaderemu-linux/toolchain/bin:$PATH"
mkdir -p "$OUT"
for f in hlsl hlsl_parse hlsl_gen hlsl_back hlsl_draw; do
    rv32-cc -O2 -Wall -fno-math-errno -I"$HERE" -I"$REPO/programs/mc" -c "$HERE/$f.c" -o "$OUT/$f.o"
done
rm -f "$OUT/libhlsl.a"
riscv32-linux-ar rcs "$OUT/libhlsl.a" "$OUT"/hlsl.o "$OUT"/hlsl_parse.o "$OUT"/hlsl_gen.o "$OUT"/hlsl_back.o "$OUT"/hlsl_draw.o
riscv32-linux-strip -g "$OUT/libhlsl.a"
ls -l "$OUT/libhlsl.a"
