#!/bin/bash
# build.sh check's compiler: clang for riscv32 standing in for gpuvm. `-c` of a C file compiles it to assembly only
# (the inline asm's text is gpu_vm's and is not assembled) and leaves an empty object; `-c` of a .S file preprocesses
# it only. Everything else (asm-offsets' -S, -E, Kconfig's and Kbuild's probes) is clang's.
set -u
CLANG="${STANDIN_CLANG:-clang}"
args=("$@")
out="" src="" compile=0
for ((i = 0; i < ${#args[@]}; i++)); do
    a="${args[$i]}"
    case "$a" in
    -c) compile=1 ;;
    -o) out="${args[$((i + 1))]}" ;;
    *.c|*.S) src="$a" ;;
    esac
done
if [ $compile = 0 ] || [ -z "$out" ] || [ -z "$src" ]; then
    exec "$CLANG" "$@"
fi
rest=()
for ((i = 0; i < ${#args[@]}; i++)); do
    a="${args[$i]}"
    case "$a" in
    -c) ;;
    -o) i=$((i + 1)) ;;
    *) rest+=("$a") ;;
    esac
done
case "$src" in
*.c) "$CLANG" "${rest[@]}" -fno-integrated-as -S -o "$out.s" || exit 1 ;;
*.S) "$CLANG" "${rest[@]}" -E -o "$out.i" || exit 1 ;;
esac
exec "$CLANG" --target=riscv32-unknown-linux -c -x c /dev/null -o "$out"
