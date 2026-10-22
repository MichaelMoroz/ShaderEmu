#!/usr/bin/env bash
# Types commands into image.sh's machine on gvm_cpu (or the card's gvm_gpu) and prints what came out
# (gpu_vm/userland/apps/README.md, "Running"): `stty -echo` first and `poweroff -f` last are added.
#   bash gpu_vm/userland/apps/run.sh [cpu|gpu] 'command' ...        (WSL for cpu; Git Bash through the queue for gpu)
#   CMDS=FILE: the commands from a file, a line each. LOG: the output (build/gvm_apps/img/run-MACHINE.log).
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
W=${W:-$ROOT/build/gvm_apps/img}
MACHINE=cpu
case "${1:-}" in cpu|gpu) MACHINE=$1; shift ;; esac
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WIN=1 ;; *) WIN=0 ;; esac
case $MACHINE in
cpu) if [ $WIN = 1 ]; then EMU="$ROOT/bin/gvm_cpu.exe"; else EMU="$ROOT/build/gvm_cpu/gvm_cpu"
         make -s -C "$ROOT/gpu_vm/cpu" CC=gcc > /dev/null || exit 1; fi ;;
gpu) EMU="$ROOT/bin/gvm_gpu.exe" ;;
esac
LOG=${LOG:-$W/run-$MACHINE.log}
IN=$W/run-$MACHINE.in
{
    echo 'stty -echo'
    if [ -n "${CMDS:-}" ]; then cat "$CMDS"; fi
    for c in "$@"; do printf '%s\n' "$c"; done
    echo 'echo RUN-END'
    echo 'poweroff -f'
} > "$IN"
RAM=$(cat "$W/ram_mb" 2>/dev/null || echo 192)
start=$(date +%s.%N)
perl "$ROOT/gpu_vm/userland/drive.pl" "$LOG.prompts" "$IN" "$EMU" --stats --ram "$RAM" --rom "$W/rootfs.romfs" \
    ${GVM_ARGS:-} --dtb "$W/gvm.dtb" "$W/vmlinux" > "$LOG"
code=$?
end=$(date +%s.%N)
sed -e 's/^ *[0-9.]* //' "$LOG"
echo "run.sh: exit $code, $(awk "BEGIN {printf \"%.1f\", $end - $start}") s; $LOG"
grep -q RUN-END "$LOG" || exit 1
exit $code
