---
name: debugging-the-machine
description: Find out why something fails on the ShaderX86 machine - which harness to use for what, reading a run's console, a program's crash lines (unhandled signal, epc, ra, badaddr), turning a pc into a source line for a flat program or the kernel with addr2line, hangs and frozen programs, kernel panics, stale kernels and images, snapshots refused after a layout change, gpu_queue and shell quoting traps, and exit 75 from the harness lock. Use when a program crashes, hangs, prints nothing, behaves differently on the card than on rvc_cpu, or when a run ends strangely. Triggers on: segfault, unhandled signal, crash, panic, oops, hang, freeze, stuck, does not exit, addr2line, which pc, --uart-log, [lock] refused, exit 75, REFUSED, snapshot refused, wrong image, old kernel, why does it not work.
---

# Debugging the machine

Workers and races have their own skill (`worker-core-bugs`). Depth: `docs/machine/cpu-harness.md`, `docs/machine/multicore.md`.

## 1. Pick the harness

| Need | Use |
|---|---|
| Does it run, what did it print, instructions, sums, crashes | `bin\rvc_cpu.exe`: boots in under a second, 100 to 200 M instructions a second, no card, no GPU, no sound unless `RVC_SOUND=1` |
| What it looks like, the window, drawing, keys by hand | `bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --image linux-net --cpu --desktop`, through `tools\gpu_queue.ps1` |
| A picture with no card time | the same with `--warp`: `--cpu --warp --save-state F.snap`, then `python tools\snapshot_screen.py F.snap OUT.png`. Not `--screen-log`, which can miss a window |
| Right here, wrong on the card | `RVC_RACES` on `rvc_cpu` first, then `python tools\test_card.py CASE` |
| The tick itself | `tick-and-shader-changes` skill |

`--seconds` is the guest's time, `--wall` this computer's, `--instructions` core 0's. Always give `--wall`, and read the log before concluding anything: an `--until` that was never met is not a result.

    $env:RVC_THREADS='8'
    bin\rvc_cpu.exe --quiet --uart-log logs\x.log --wall 60 --expect "/ # " --send "nano-X -p & sleep 2; CMD; echo RC=`$?; echo LX-''DONE\n" --until LX-DONE

## 2. First: is it the build you think

More time went on this than on any bug.

- **Stale kernel.** `kernel.sh` once failed to link and copied the old `Image` on: a whole baseline ran a kernel 3 days old. It stops at a failed make now, but look: `ls -l build\nommu\Image`, and `make exit 0` in its output. A driver change needs `kernel.sh` *and* `image.py`.
- **Stale image.** A rebuilt program reaches the image only by `linux/nommu/root.sh` then `tools\make_image.ps1`. `md5sum /usr/bin/NAME.bin` in the guest says which ran.
- **The wrong folder.** A worktree has no `bin\`, `build\` or `rvc\`, and a tool there quietly lacks what it expects: name the image and the emulator.
- **A rebuilt ELF.** `prof.py report` refuses an ELF that is not the image's; addr2line does not, and gives a wrong line with confidence. Keep the ELF with the image.
- **A snapshot of a layout before** is refused (`SX86SNP2`, `RVCSNAP2`): make it again (`python tools\test_card.py mctest-3 --snapshot`). Under `--cpu`, `--load-state` restores only the texture; use `rvc_cpu --save`/`--load` there.
- **The tick**: `rvc_harness` exits 3 when `experiments\rvc_opt\warp_ok` lacks the folder's stamp. See `tick-and-shader-changes`.
- **Another agent's run or build** beside yours. `~/nm` is shared: `building-the-system` has the private home.

## 3. A program that crashed

The kernel prints this to the console (exception-trace is 1; it was run on 31 October 2026, with `cc` in the guest):

    [    6.087800] c[25]: unhandled signal 11 code 0x2 at 0x00000000
    [    6.095200] epc : 00000000 ra : 00000000 sp : 00000000
    ...
    [    6.124800] status: 80006020 badaddr: 00000000 cause: 00000001

- `cause`: 1 fetch fault, 2 illegal instruction, 5 load fault, 7 store fault. `badaddr` is the address. The shell then says `Segmentation fault` and `$?` is 139.
- **There is no memory protection.** Programs share one address space and most wild stores do not fault at all: `*(int *)0x40 = 1` returned 0 with no message, while `*(int *)0x10 = 1` ended with epc, ra and sp all 0. So a crash is often far from its cause, a store over another program's memory is possible, and registers can be nonsense. Suspect a corrupted heap or a smashed stack before the line it died at. Stack is 256 KB, or 1 MB for `BIG` in `linux/nommu/flat.py`.
- `/proc/PID/maps` shows the program's whole block as `[stack]`, then its mappings. Reading before writing depends on what the program left there before: OpenTTD's `RandomAccessFile` found a header "there" in an unset buffer, and a sound failed.
- A kill with `code 137` (`Killed`, RC 137) is out of memory: a mapping wants one run of pages. `python linux/nommu/memory.py` checks the large programs fit.
- No core dumps exist (`CONFIG_COREDUMP` is off). `RVC_TRAP_LOG=N` on `rvc_cpu` prints the first N traps with pc and value; `RVC_CPU_LAST=1` keeps core 0's last instructions for the summary; `--save F` keeps the machine to go on from.
- A trap inside musl's `malloc` or `free` is its assert: read it as a heap fault (a kept group reused without clearing crashed Doom, Quake and `cc` so).

### pc to source

    A flat program:  ELF address = pc - (start of its [stack] line in /proc/PID/maps) - 0x40
    The kernel:      vmlinux is linked at 0x82000000: the pc as it is

Checked on Doom: map start `83800000`, pc `83833b04` holds the word of `P_CheckPosition`+0, ELF `0x33ac4`. Placement goes by what is free, so read the map in the same run: `CMD & sleep 3; cat /proc/$!/maps`. `prof.py report` prints the same base (`doom: at 0x83800040`).

In WSL, with the build's unstripped ELF (`~/nm/work/src/NAME-build/NAME`, `~/nm/work/src/apps/NAME`, `~/nm/k-rom/vmlinux`):

    ~/shaderemu-linux/toolchain/bin/riscv32-linux-addr2line -f -i -e ELF 0xADDR

Without `-g` the line is `:?`, the function name is still right. `-i` shows what LTO inlined. Use `ra` when epc is 0.

## 4. A hang or a freeze

Find out whose: is the guest's clock moving, does the shell answer?

1. Reproduce on `rvc_cpu` with `--wall`, then look at its last lines:
   - `CORE n: pc ...` for each core, and the instructions each ran. Name that pc as above.
   - `[cpu] traps:` and `[cpu] stops: workers' faults N`. A worker stopped at a page stops whoever waits for it.
   - `[cpu] the program with the keyboard:`.
2. `RVC_PASSLOG=FILE` with `--count-from`: is it spinning (`end` 0, same pc), or waiting (2)? A wait in place on the clock burns the pass; the clock moves only between passes.
3. A second command through `--expect TEXT --send "..."`: `/proc/PID/status`, `cat /proc/gpu` (who holds the GPU: `pid workers wanted lent tag draws lists`), `ps` are. "One program at a time draws": a program that did not exit makes the *next* one fail to start, so the visible error can be another program's.
4. A guest with a window: `nxmon`'s Processes tab shows who takes the first core. A desktop hang wants `--desktop` and `nxkey` to reproduce, since a bare `nano-X -p` differs (the OpenTTD exit bug was on the real desktop).
5. Timing-dependent: `--threads 1` gives the same run each time. `--save-after S --save F` then `--load F` goes on from just before it, to the same bytes.
6. Only on the card or only with its memory: `RVC_RACES`, `RVC_BESIDE=1`.

A frozen level, a game stuck after a load: usually a worker wanted something only core 0 may do. See `worker-core-bugs`.

## 5. A panic or an oops

`CONFIG_KALLSYMS` is off, so the console has addresses alone: `riscv32-linux-addr2line -f -e ~/nm/k-rom/vmlinux 0xPC` for `epc` and `ra`. Nothing printed at all: the log is a 16 KB ring, and the console is a ring of 256 bytes a host clock value (`patch_console.py`).

- Boot stops at the start: was the image made for this layout? RAM is 0x82000000 to 0x92000000 and `tools/make_linux_image.py` and `image.py` place the device tree. `getrandom()` never returning means no `rng-seed` (`image.py` writes it).
- A hook's edit that is not there: a hook skips a file with its marker. `git checkout` the file in the kernel tree, or `kernel.sh` from the start (`building-the-system`).
- `RVC_FAST_CHECK=1` runs each pass by the fast handlers and the general step and stops at the first that differs (exit 4): an emulator bug in `rvc_cpu` rather than the guest.

## 6. It differs on the card

Expected differences: float programs' sums (the card's division within one unit in the last place, denormals flushed, `docs/machine/fpu.md`), so `CARD` in `tools/test_card.py` keeps its own values. `nxpath-balls`' value looked like a card fault and was a value never updated after a libm change: prove it by building the program with the old function on both before changing a value.

Anything else: races first, then stores a pass later (`RVC_BESIDE=1`), then a capture (`tools\gpu_reference.py SNAP BMP --size WxH` on a `--gpu-capture`).

## 7. Quoting and the lock

- Start `rvc_cpu` from PowerShell. Git Bash rewrites `"/ # "` into a path.
- In PowerShell `$` in a double-quoted `--send` wants a backtick: `` echo `$? ``, `` cat /proc/`$!/maps ``. Unescaped, `$!` is PowerShell's and is sent empty.
- `echo LX-''DONE` keeps the echoed command from matching `--until`. Inside a single-quoted PowerShell string write `''''`: `''` collapses to one quote, the shell then waits for the closing one, and the run ends at its wall with no message.
- `\n`, `\r`, `\t` are `--send`'s only escapes on `rvc_cpu`; the harness has `\xNN` too. A real newline in C source sent this way breaks the string: use `%c` with 10.
- `pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe ...` keeps its arguments whole. Called from another process use `pwsh -NoProfile -File`. `all.ps1 -Builds a,b` from another process does not split: call `linux/nommu/build.sh a b`.
- A long typed line is folded on the console at 80 columns in the log: a `--until` text must not span the fold; keep the mark short.
- A Bash-tool heredoc loses a level of backslashes; Python editing a repository file opens `rb`/`wb`, since text mode writes CRLF. In WSL `$NAME` is the computer's name.

**Exit 75**: `[lock] rvc_harness refused (GPU mode): held by CPU harness rvc_cpu pid N`. Any number of CPU harnesses run together, or one GPU harness alone; it does nothing and does not wait. Under the queue it says `gpu_queue: REFUSED by the harness lock, nothing ran`. Wait and run again (`game_par.py` and `test_card.py` do), and do not take it for a failed test. `gpu_queue: the card is taken, waiting:` prints whose turn it is; `$env:GPU_QUEUE_WHO='name'` says yours.

`rvc_harness.exe` with no arguments is the developer's own and holds the card: leave it, and do not time beside it.
