#!/bin/bash
# Where a program's bundles go: runs it on gvm_cpu with --trace and counts the bundles run by their number of ops
# (0, 1, 2) and the hottest bundles with their functions. In WSL:
#   bash gpu_vm/llvm/test/bench/profile.sh PROGRAM.elf [TOP]
set -u
ELF=$1
TOP=${2:-25}
ROOT=$(cd "$(dirname "$0")/../../../.." && pwd)
BIN=${GVM_LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}
"$ROOT/build/gvm_cpu/gvm_cpu" --trace "$ELF" 2>&1 >/dev/null | awk '
    /^\[[0-9]+\] / {
        ctl = strtonum("0x" $3); n = int(ctl / 8) % 4; cc = int(ctl / 128) % 16
        ops[n]++; total++; pc[$2]++
        if (n == 0 && cc != 5) branchonly++
    }
    END {
        printf "bundles %d: 0 ops %d (%.1f%%; with a branch or a control op %d), 1 op %d (%.1f%%), 2 ops %d (%.1f%%); %.2f ops a bundle\n",
            total, ops[0], 100 * ops[0] / total, branchonly, ops[1], 100 * ops[1] / total, ops[2], 100 * ops[2] / total,
            (ops[1] + 2 * ops[2]) / total
        for (p in pc) print pc[p], p > "/tmp/gvm_profile_pcs"
    }'
sort -rn /tmp/gvm_profile_pcs | head -"$TOP" > /tmp/gvm_profile_top
"$BIN/llvm-objdump" -d -z --no-show-raw-insn "$ELF" > /tmp/gvm_profile_dis
while read -r count pc; do
    a=$(printf '%x' "0x$pc")
    line=$(grep -m1 "^ *$a:" /tmp/gvm_profile_dis | sed 's/^ *[0-9a-f]*:\s*//')
    fn=$(awk -v a="$a" '/^[0-9a-f]+ <.*>:$/ { f = $2 } $1 == a ":" { print f; exit }' /tmp/gvm_profile_dis)
    printf '%8d %s %-24s %s\n' "$count" "$a" "$fn" "$line"
done < /tmp/gvm_profile_top
