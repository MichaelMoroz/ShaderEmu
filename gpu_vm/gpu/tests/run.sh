#!/bin/sh
# gvm_gpu's tests: gvm_cpu's cases (gpu_vm/cpu/tests/run.sh) on the card. Run the whole script as one GPU run:
#   pwsh tools\gpu_queue.ps1 sh gpu_vm/gpu/tests/run.sh bin/gvm_gpu.exe DIR [OPTIONS...]
# DIR holds gvm_cpu_mk's programs (bin\gvm_cpu_mk.exe DIR); OPTIONS go to every run (--check, --shader F, --warp).
# Prints each case and exits 1 if any failed.
gpu=$1; d=$2; shift 2; opts="$*"; bad=0
check() {   # name, expected exit code, expected stdout, command...
    name=$1; code=$2; want=$3; shift 3
    got=$("$@" 2>"$d/$name.gpu.err"); rc=$?
    if [ "$rc" = "$code" ] && [ "$got" = "$want" ]; then echo "ok   $name"
    else echo "FAIL $name: exit $rc (want $code), stdout '$got' (want '$want')"; cat "$d/$name.gpu.err"; bad=1; fi
}
g() { "$gpu" $opts "$@"; }
check hello 0 "Hello from gpu_vm!" g "$d/hello.elf"
check sum 186 "" g "$d/sum.elf"
check sum-bin 186 "" g --bin "$d/sum.bin" --at 0x02000000
check sum-short-passes 186 "" g --budget 1 "$d/sum.elf"
check echo 6 "hello" sh -c "printf 'hello\n' | '$gpu' $opts '$d/echo.elf'"
check args 3 "second" g "$d/args.elf" second third
check fault 2 "" g "$d/fault.elf"
check break 2 "" g "$d/break.elf"
check handler 2 "" g "$d/handler.elf"
check timer 43 "" g "$d/timer.elf"
check max-bundles 3 "" g --max-bundles 1000 "$d/spin.elf"
check opasync 7 "" g "$d/opasync.elf"
check opasync-short 7 "" g --budget 3 "$d/opasync.elf"
check console 120 "hi
ok
x" sh -c "printf x | '$gpu' $opts '$d/console.elf'"
check console-no-stdin 3 "hi
ok" g --no-stdin --max-bundles 100000 "$d/console.elf"
digits=$(i=0; while [ $i -lt 600 ]; do printf $((i % 10)); i=$((i + 1)); done)
check conwrap 0 "$digits" g "$d/conwrap.elf"
check boot 42 "" g --dtb "$d/boot.dtb" "$d/boot.elf"
check boot-mem32 42 "" g --dtb "$d/mem32.dtb" "$d/boot.elf"
grep -q "the memory node says 32 MB, --ram 64" "$d/boot-mem32.gpu.err" || { echo "FAIL boot-mem32: no warning"; bad=1; }
check strace 0 "Hello from gpu_vm!" g --strace --stats "$d/hello.elf"
grep -q "syscall 64(0x1, 0x2010000, 0x13) = 19" "$d/strace.gpu.err" || { echo "FAIL strace: no syscall line"; bad=1; }
exit $bad
