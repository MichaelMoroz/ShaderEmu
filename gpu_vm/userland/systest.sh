#!/bin/bash
# The system test (gpu_vm/userland/README.md, "The system test"): boots rootfs.sh's kernel (build/gvm_rootfs), types a
# session of commands into the console (the emulator's stdin), checks what came out and says how long each command
# took on the wall clock. The last command powers the machine off, which ends the emulator.
#   bash gpu_vm/userland/systest.sh [cpu|gpu] [EMULATOR OPTIONS...]
# cpu: gvm_cpu (WSL: build/gvm_cpu/gvm_cpu, made first; Git Bash: bin\gvm_cpu.exe). gpu: bin\gvm_gpu.exe, from Git
# Bash and through the card's queue:
#   pwsh tools\gpu_queue.ps1 "C:\Program Files\Git\bin\bash.exe" gpu_vm/userland/systest.sh gpu [--check]
# SESSION=short: a few commands only (for --check, which runs the reference beside the card). SESSION=FILE: a file of
# commands (no checks but that the machine powered off). LOG: the output (build/gvm_rootfs/systest-MACHINE.log; the
# console's output with each line's wall time, and the commands' times in .times).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
W="$ROOT/build/gvm_rootfs"
MACHINE="${1:-cpu}"; shift || true
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WIN=1 ;; *) WIN=0 ;; esac
case "$MACHINE" in
cpu) if [ $WIN = 1 ]; then EMU="$ROOT/bin/gvm_cpu.exe"; else EMU="$ROOT/build/gvm_cpu/gvm_cpu"
         make -s -C "$ROOT/gpu_vm/cpu" CC=gcc > /dev/null || exit 1; fi ;;
gpu) EMU="$ROOT/bin/gvm_gpu.exe" ;;
*) echo "systest.sh: cpu or gpu" >&2; exit 1 ;;
esac
SESSION="${SESSION:-full}"
LOG="${LOG:-$W/systest-$MACHINE.log}"
for f in vmlinux gvm.dtb busybox; do [ -f "$W/$f" ] || { echo "systest.sh: no $W/$f (rootfs.sh)" >&2; exit 1; }; done
BBMD5=$(md5sum "$W/busybox" | cut -d' ' -f1)

# the session: one command a line; `stty -echo` first, so that what is typed ahead is not echoed into the output
full=(
    'stty -echo'
    'uname -a'
    'cat /proc/version'
    'ls /'
    'echo T42=$((6*7))'
    "seq 1 5 | awk '{s+=\$1} END {print \"SUM=\" s}'"
    'cat /proc/cpuinfo'
    'head -3 /proc/meminfo'
    'free'
    'df /tmp'
    'mount'
    'ps'
    'date'
    'uptime'
    'id'
    'hostname'
    'echo hello | gzip | gunzip'
    'echo hello2 | minigzip | minigzip -d'
    "lua -e 'print(2^10, _VERSION)'"
    "lua -e 'local t = {} for i = 1, 10 do t[i] = i * i end print(\"LUA=\" .. table.concat(t, \",\"))'"
    'md5sum /bin/busybox'
    'sha256sum /etc/passwd'
    "printf 'b\\na\\nc\\na\\n' | sort | uniq -c | sed 's/^ *//' | tr '\\n' ' '; echo"
    'echo abc | sed s/b/X/'
    'grep ^root /etc/passwd'
    'find /etc -type f | wc -l'
    'cd /tmp && tar cf etc.tar /etc && tar tf etc.tar | wc -l && gzip etc.tar && ls -l etc.tar.gz && gunzip etc.tar.gz && md5sum etc.tar | cut -c1-8; cd'
    "mkdir -p /tmp/d/e && cp /etc/motd /tmp/d/m && mv /tmp/d/m /tmp/d/n && ls /tmp/d | tr '\\n' ' ' && rm -r /tmp/d && echo RM=\$?"
    'du -s /etc'
    'head -1 /etc/passwd; tail -1 /etc/group'
    'echo a b c | xargs -n1 echo X'
    'hexdump -C /etc/hostname'
    'od -c /etc/hostname'
    'diff /etc/hostname /etc/hostname && cmp /etc/hostname /etc/hostname && echo SAME'
    'echo EXPR=$(expr 3 + 4) TEST=$(test -d /proc && echo yes)'
    'env | grep ^PATH'
    'f() { echo "FN:$1"; }; f arg; for i in 1 2 3; do echo -n "L$i "; done; echo; (cd /tmp; echo "SUB:$(pwd)")'
    'sleep 5 & kill $!; wait $!; echo KILLED=$?'
    'sleep 1 & jobs; wait; echo JOBS-DONE'
    'time ls /bin > /dev/null'
    'dmesg | tail -2'
    'top -b -n 1 | head -4'
    'gvm-tests'
    'coremark 0x0 0x0 0x66 20'
    'echo SYSTEST-END'
    'poweroff -f'
)
short=(
    'stty -echo'
    'uname -a'
    'echo T42=$((6*7))'
    "seq 1 5 | awk '{s+=\$1} END {print \"SUM=\" s}'"
    'echo SYSTEST-END'
    'poweroff -f'
)
case "$SESSION" in
full) cmds=("${full[@]}") ;;
short) cmds=("${short[@]}") ;;
*) mapfile -t cmds < "$SESSION" ;;
esac
IN="$W/systest-$MACHINE.in"
printf '%s\n' "${cmds[@]}" > "$IN"

# the run: each line of the output with its wall time, and the time each prompt appeared
STAMP='$| = 1; use Time::HiRes "time"; my $t0 = time; my ($buf, $tail, $bol) = ("", "", 1);
    open(my $pt, ">", $ARGV[0]) or die;
    while (sysread(STDIN, $buf, 4096)) {
        my $t = time - $t0; $buf =~ s/\r//g;
        $tail .= $buf; while ($tail =~ s/^.*?\@gpuvm:[^\n]*?# //s) { printf $pt "%.3f\n", $t }
        $tail = substr($tail, -64) if length($tail) > 64;
        for my $c (split /(?<=\n)/, $buf) { printf "%9.3f ", $t if $bol; print $c; $bol = ($c =~ /\n$/) }
    }'
echo "systest.sh: $MACHINE, ${#cmds[@]} commands; the output in $LOG"
start=$(date +%s.%N)
"$EMU" --stats "$@" --dtb "$W/gvm.dtb" "$W/vmlinux" < "$IN" 2>&1 | perl -e "$STAMP" "$LOG.prompts" > "$LOG"
code=${PIPESTATUS[0]}
end=$(date +%s.%N)
echo "systest.sh: the emulator's exit code $code, $(awk "BEGIN {printf \"%.2f\", $end - $start}") s"

# the commands' times: from the prompt where each was typed to the next prompt
mapfile -t pt < "$LOG.prompts"
{
    printf '%-9s %s\n' "boot" "${pt[0]:-?} s to the first prompt"
    for ((i = 0; i + 1 < ${#pt[@]} && i < ${#cmds[@]}; i++)); do
        printf '%8.3f  %s\n' "$(awk "BEGIN {print ${pt[$((i + 1))]} - ${pt[$i]}}")" "${cmds[$i]}"
    done
} > "$LOG.times"
cat "$LOG.times"

# the checks
fail=0
check() { if grep -qE -- "$1" "$LOG"; then echo "  ok    $1"; else echo "  FAIL  $1"; fail=1; fi; }
echo "systest.sh: checks"
check 'Run /init as init process'
check 'Linux gpuvm 5\.17\.11'
check 'T42=42'
check 'SUM=15'
check 'SYSTEST-END'
check 'reboot: (System halted|Power down)'
if [ "$SESSION" = full ]; then
    check 'gpuvm: up in'
    check 'hello$'
    check 'hello2$'
    check '1024\.0[[:space:]]+Lua 5\.'
    check 'LUA=1,4,9,16,25,36,49,64,81,100'
    check "$BBMD5  /bin/busybox"
    check '2 a 1 b 1 c'
    check 'aXc'
    check 'root:x:0:0:root:/root:/bin/sh'
    check 'etc\.tar\.gz'
    check 'e n RM=0'
    check 'RM=0'
    check 'X a'
    check '67 70 75 76 6d 0a'
    check 'SAME'
    check 'EXPR=7 TEST=yes'
    check 'PATH=/bin:/sbin:/usr/bin:/usr/sbin'
    check 'FN:arg'
    check 'L1 L2 L3'
    check 'SUB:/tmp'
    check 'KILLED=143'
    check 'JOBS-DONE'
    check 'real[[:space:]]+[0-9]'
    check 'gvm-tests: 5 passed, 0 failed'
    check 'seedcrc +: 0xe9f5'; check 'crcfinal +: 0x'; check 'Iterations/Sec'
    check 'Mem:'
    check 'tmpfs'
fi
[ $code = 0 ] || { echo "systest.sh: the emulator's exit code is $code"; fail=1; }
if [ $fail = 0 ]; then echo "systest.sh: PASS ($MACHINE)"; else echo "systest.sh: FAILED ($MACHINE; $LOG)"; exit 1; fi
