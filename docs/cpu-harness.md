# The machine on the processor

`bin\rvc_cpu.exe` (`harness/apps/rvc_cpu.cpp`) is the machine as an ordinary interpreter. The
shader machine runs three to four million instructions a second; what a program costs on it
is a number of instructions, and that number does not depend on what runs them. So work that
is about instructions (a game's logic, a frame's cost, a state sum, a test that a program
still runs) does not need the graphics card. The interpreter runs about a hundred million
instructions a second on one thread: Red Alert's check, from the shell prompt through the
window system's start, the opening movie on three worker cores and 300 frames of a mission,
is 5 seconds here and 90 in `rvc_harness`, with the same state sum.

    bin\rvc_cpu.exe --cores 4 --quiet --uart-log logs\uart_x.log --expect "/ # " ^
        --send "nano-X -p & sleep 2; TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=300 ralert; echo LX-''DONE\n" ^
        --until "LX-DONE"

It boots the image in `build\images\linux` from power-on every time (a boot is 13 million
instructions, a tenth of a second), takes the harness's `--expect`, `--send`, `--until`,
`--uart-log`, `--cores`, `--ticks`, `--pointer-sweep`, `--desktop` and `--tabs`, and ends with
what each core ran, the firmware calls and the traps by cause. `--seconds` is the guest's
time, `--wall` this computer's. Start it from PowerShell or cmd: Git Bash turns `"/ # "` into
a path.

## What it is

- **The processor**: RV32IMA with the float instructions (the host's own floats, which are
  IEEE single precision too; `fptest` passes), supervisor and user mode, Sv32 with the
  accessed and written bits set by the walk, and a table of the last pages a core used for
  each of fetch, read and write. No machine mode: as in the shader machine with `SBI_HLE`,
  every trap is the supervisor's and its `ecall` is answered in place (timer, console, the
  base extension).
- **Memory**: 128 MB from `0x80000000` (Linux's RAM, then the GPU's and the control words, which
  are RAM), the ROM at `0x40000000`, the parallel copy and fill (CSRs `0x0b0` to `0x0b3`, whose
  source may be the ROM), the clock chip and a UART that prints.
- **A pass** is `--ticks` instructions (16,384), or fewer when the core waits (`wfi`, `pause`) or
  asks for a copy, as on the shader machine, and the host's part of the machine runs between
  passes: the clock word, the host flags, the pointer, what each core ran, and the GPU's
  submit word.
- **The worker cores** (`docs/multicore.md`) follow the same mailbox: a start texel, sleeping
  on the job word, stopping at a fault or a system call until core 0 answers. They run one
  after another inside a pass and see each other's stores at once; on the shader machine a
  store is seen a pass later. A program that is right there is right here, not the other way
  round: `mctest` and the games' own checks belong to `rvc_harness`.
- **The guest's clock** is counted, not measured: a pass takes `--pass-ms` (0.25) and its
  instructions `1 / --ips` seconds each (3,700,000), which is about what the harness's
  machine does; an idle machine goes straight to its timer's next deadline. A game's
  "frames a second" printed under it are therefore the shader machine's, roughly; its
  instructions a frame are exact.

## What it is not

- **No GPU, no display, no sound card.** A submitted list is taken as drawn (the submit word
  cleared, the counters counted), and a picture a program asked to have copied back into its
  window is not there. Those devices are cheap on the graphics card and stay there: pictures
  and sound are checked with `rvc_harness`. (Running them from this program's memory, as the
  harness's passes, is the plan for when a picture is wanted at this speed.)
- No keys or pointer buttons yet (only `--pointer-sweep`), no pages from the host
  (`docs/fetch.md`), no snapshots.
- Not a model of the shader machine's limits: no write cache that fills, no one-writer-per-16
  -bytes rule for workers. It answers "how many instructions", not "does it run there".

## Checking it

`fptest` in the guest (every float instruction against the runtime's integer routines), the
games' state sums (`docs/ralert.md`, `docs/tdawn.md`), and the workers' instruction counts,
which for Red Alert's movie agree with the shader machine's to five digits.
