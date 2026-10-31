# The desktop's fuzzer

`tools/app_fuzz.py` drives every program of the Start menu under `nx` on `rvc_cpu`: opened together, used by keys,
clicks, drags and the wheel, closed by every way a player has. Each case is a new machine (it boots in under a second)
running one *plan*, a list of `nxfuzz`'s operations (`linux/apps/nxfuzz.c`, which says them all), so a failure replays
from its line.

    python tools/app_fuzz.py list
    python tools/app_fuzz.py plan "start:a:nxpaint wait:a:30 rand:a:20:7 close:a:x end"    # one plan, console printed
    python tools/app_fuzz.py single [APP ...]    # each program, each way of closing: at once, during its start, after a use
    python tools/app_fuzz.py pairs [APP ...]     # every pair open together, in turn closed in either order; then Gears must start
    python tools/app_fuzz.py triples --light     # the same with three of the small programs
    python tools/app_fuzz.py random --seed 1 --count 200 [--no-games]
    python tools/app_fuzz.py file PLANS.txt      # a plan a line
    python tools/app_fuzz.py shrink "PLAN"       # as few operations as still fail the same way

`--image DIR` (default `build/images/nommu`), `--jobs N` (3), `--threads N` (8), `--wall S` a plan, `--races` (`RVC_RACES`),
`--log DIR`. The card's model is on (`RVC_WCACHE`, `RVC_ADFAULT`, `RVC_SOUND`; `--no-card-model`). A run refused beside a
GPU run (exit 75) is run again. Results are lines of `build/app_fuzz/results.jsonl`, a failed plan's console beside them.
The guest's clock is the counted one, so ten minutes of play are some ten seconds of this computer's.

## How it closes

`close:TAG:HOW` is `x` (a real click on the frame's button), `alt` (a click on its caption and Alt+F4), `bar` (what the
taskbar's Close sends), `term` (Monitor's End Process), `kill`, `twice`; `tell:TAG:HOW` the same with no waiting, so
that a question can be answered by a click (OpenTTD's Yes is `click:o:367,261` in a window of 640 x 480).

A program that stays with its window up is one with a question, and that is no fault: after `ask` seconds (12) it gets
Enter, then Alt+F4 and Enter, then SIGTERM; only one still there after that is `no-close`. A process that goes on with
its window gone is `left-running`, `gone_wait` seconds (40) after: the report that started this.

## What fails a plan

- `FZ-BAD` lines: `left-running`, `no-close`, `window-of-dead` (a window whose process has gone), `no-window`, `gpu-of-dead`
  (`/proc/gpu` names a process that has gone), `gpu-holder` and `left-process` (after `end`, against what was running before
  the plan), `canary` (a program started and waited for), `hang` (an operation over its alarm: 240 guest seconds).
- What the console says: an oops or a panic, a segfault, `a worker wanted`, `Sys_Error`, `Could not start`, a crash, an
  assertion, an out of memory. "One program at a time" is a failure only if the program it names has been closed: while
  it is open a second program is meant to be told so, and then to have no window.
- `RVC_RACES` undone with `--races`; no `FZ-DONE` within the wall (`wall`: inconclusive by itself, `--races` is many times slower).

A window goes to a tag when its process is in the session its `start:` made; a question (`nxask`) is said and goes to nobody.

`end` closes each tag, then whatever nobody started (random keys can start a program from the desktop's icons, and a
question is a program of its own, `nxask`), and compares with the start. Memory is said (`FZ memory`), not judged.

## Limits

- `rvc_cpu` draws no GPU list, so a wait for the GPU is never long there. `rvc_harness --cpu --warp` runs the same line with
  the lists drawn (some 5 M instructions a second), through `tools\gpu_queue.ps1`.
- Random clicks do not know a program's menus: a menu's far end is reached rarely. What each program's own switches
  and tests reach is in `tools/test_all.py`.

## What it found

- OpenTTD's link by `fork` and `exit()`, and what was done about a program that stays: "At exit" in `docs/games/openttd.md`.
- A child kept the GPU's file (`open` for that link, `nxask` from a refused program) and with it the drawing and the workers' places of
  a program that had gone: `shaderemu_gpu_close_on_exec` in the kernel's driver sets close-on-exec at a file's first ioctl or mmap.
- A child kept the window system's socket: a refused program's empty window stayed until its Notice was answered, and OpenTTD's until `open`'s
  40 s were over. `nxask` and `open` close what they were given above 2 first.
