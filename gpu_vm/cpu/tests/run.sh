#!/bin/sh
# gvm_cpu's tests: tests/run.sh GVM_CPU DIR (DIR holds mk's programs). Prints each case and exits 1 if any failed.
cpu=$1; d=$2; bad=0
check() {   # name, expected exit code, expected stdout, command...
    name=$1; code=$2; want=$3; shift 3
    got=$("$@" 2>"$d/$name.err"); rc=$?
    if [ "$rc" = "$code" ] && [ "$got" = "$want" ]; then echo "ok   $name"
    else echo "FAIL $name: exit $rc (want $code), stdout '$got' (want '$want')"; cat "$d/$name.err"; bad=1; fi
}
check hello 0 "Hello from gpu_vm!" "$cpu" "$d/hello.elf"
check sum 186 "" "$cpu" "$d/sum.elf"
check sum-bin 186 "" "$cpu" --bin "$d/sum.bin" --at 0x02000000
check sum-short-passes 186 "" "$cpu" --budget 1 "$d/sum.elf"
check echo 6 "hello" sh -c "printf 'hello\n' | '$cpu' '$d/echo.elf'"
check args 3 "second" "$cpu" "$d/args.elf" second third
check fault 2 "" "$cpu" "$d/fault.elf"
check break 2 "" "$cpu" "$d/break.elf"
check handler 2 "" "$cpu" "$d/handler.elf"
check timer 43 "" "$cpu" "$d/timer.elf"
check max-bundles 3 "" "$cpu" --max-bundles 1000 "$d/spin.elf"
check opasync 7 "" "$cpu" "$d/opasync.elf"
check opasync-short 7 "" "$cpu" --budget 3 "$d/opasync.elf"
check trace 0 "Hello from gpu_vm!" "$cpu" --trace --strace --stats "$d/hello.elf"
grep -q "syscall 64(0x1, 0x2010000, 0x13) = 19" "$d/trace.err" || { echo "FAIL trace: no syscall line"; bad=1; }
exit $bad
