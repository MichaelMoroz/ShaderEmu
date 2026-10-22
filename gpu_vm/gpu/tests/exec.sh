#!/bin/sh
# The code generator's execution tests (gpu_vm/llvm/test/exec, built by its run.sh into build/gvm_exec) on a runner:
#   pwsh tools\gpu_queue.ps1 sh gpu_vm/gpu/tests/exec.sh bin/gvm_gpu.exe [OPTIONS...]
#   sh gpu_vm/gpu/tests/exec.sh bin/gvm_cpu.exe
# A test passes when it exits 0 and prints NAME.out when there is one. EXEC=DIR for another build.
run=$1; shift; opts="$*"
here=$(cd "$(dirname "$0")" && pwd)
src=$here/../../llvm/test/exec
d=${EXEC:-$here/../../../build/gvm_exec}
pass=0; fail=0
for exe in "$d"/*.elf; do
    n=$(basename "$exe" .elf); t=${n%-O*}
    "$run" $opts --max-bundles 400000000 "$exe" > "$d/$n.gpu.stdout" 2> "$d/$n.gpu.err"; r=$?
    ok=1
    [ $r = 0 ] || ok=0
    if [ $ok = 1 ] && [ -f "$src/$t.out" ] && ! cmp -s "$src/$t.out" "$d/$n.gpu.stdout"; then ok=0; fi
    if [ $ok = 1 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); echo "FAIL $n: exit $r"; tail -5 "$d/$n.gpu.err"; fi
done
echo "exec: $pass passed, $fail failed"
[ $fail = 0 ]
