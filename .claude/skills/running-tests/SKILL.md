---
name: running-tests
description: Run ShaderX86's test sets and read them - tools/test_all.py (tags, --races, --long, --sections, --card-model, --doubles, --cpu, --image, --logs, walls), tools/test_card.py (--parts, --warp, --mark-ok, --snapshot), running from a git worktree, adding a case, setting a value moved on purpose, and what to do with a failure. Use after a change, before a merge, when a case fails or times out, when adding a test to a program, or when asked whether something still passes. Triggers on: test_all, test_card, the test set, a case failed, cc-bytes, --races, --parts, WARP, CC_BYTES_NOMMU, CARD table, regression, FAIL.
---

# Running the tests

Depth: `docs/machine/cpu-harness.md` ("The test set"). Comments at the head of `tools/test_all.py` list a case's checks.

## Which, when

| After | Run |
|---|---|
| Any change | `python tools/test_all.py` (152 cases, about two minutes alone; 42 to 64 s was seen on an idle computer) |
| Work on workers | and `python tools/test_all.py --races`: 0 undone, near counts beside each PASS |
| The kernel or memory | and `--long` (161: no workers, `zerotest`, `tailtest`, three bots) |
| A tick or something the card alone shows | `python tools\test_card.py machine doom-3 nxpath-balls nxpaint` |
| One program | a tag or names: `python tools/test_all.py games nes-cells`; `--list` names them. Tags include `machine games media programs desktop workers doom quake quake3 tdawn ralert classicube openttd nes web ide long` |
| A game's soft doubles | `python tools/test_all.py --doubles` (run it again to go on) |

## From a worktree

A worktree has no `bin\` or `build\`, so name what the run needs. This was run on 31 October 2026:

    python tools/test_all.py mctest-3 fptest --cpu D:\Development\ShaderX86\bin\rvc_cpu.exe --image D:\Development\ShaderX86\build\images\nommu --logs C:\...\scratchpad\ta

The tests then test *that* image, not the worktree's sources: a change to the program needs a private image. The `--logs` folder keeps from writing the checkout's `build\test_all`. `times.json` there orders the longest cases first.

## Beside other work

- `$env:RVC_THREADS='8'` when others run; `--jobs N` for fewer cases at once. A wall that passes alone may not beside a kernel build: `--wall-scale 2`, or `--max-wall 55`.
- Under a limit of a minute a call: `--sections` (40 s parts, `--section-secs`, `--call-secs`), run the same line again until no `part` is left.
- A GPU run is refused while CPU ones run (exit 75, `debugging-the-machine`): `test_card.py` waits for it by itself, up to 20 minutes.

## Reading a failure

The console of each case is `LOGS\NAME.log`; `NAME.log.races` for `--races`. One line says why. Then:

1. **Is it the tree or the environment?** Run the case on the baseline's image and emulator. `cc-bytes` once failed at every agent's baseline for a week: the digest went by the build folder's name. It was fixed by making builds independent of it, not by a new value. Find the cause of any value before overwriting it.
2. **A FAIL at the wall with no error** is a slow run or a hang: `--wall-scale 2` tells them apart; a hang goes to `debugging-the-machine`.
3. **A flaky case.** A case using fixed ports (`nxweb-http`: 47310 and 47320) fails beside another run on them. A program's timing shifts instructions, not sums. A sum that moves between runs is a bug: run it three times under `--threads 1` and without.
4. **A case passing while its work is broken.** They hold what they say and no more; captures are not in them. A form rendered blank passed every count, and a kernel 3 days old passed all: `building-the-system`.
5. Two fail from one cause more often than not: "One program at a time draws", a program left from a previous case's kill.

## A value moved on purpose

Expected values are in the script, each with the doc it comes from. Set it with a comment saying why and which commit, in the doc too: `CC_BYTES_NOMMU`'s comment is the pattern. Moved by `gles.c`, `mcw.c`, Nano-X's library, musl or an example: `cc-bytes` always moves (`linux/tcc/build.sh` first). `tailtest`'s sum goes by `/usr/share/rom-dates`, written anew by every image.

Never change a value only because the test says otherwise: `nxpath-balls`' `CARD` value was proved by building with the old float functions on both machines.

## Adding a case

    case('NAME', 'tags', 'CMD; CMD', workers, sums=[(r'regex (hex)', ['sum1', 'sum2'])]),

`X = 'nano-X -p & sleep 2; '` before a windowed run. Checks: `has=`, `never=`, `count=`, `digest=` (md5 of matched lines), `same=` (another case's lines), `sums=`; and `until=`, `wall=`, `more=['--send', ...]`, `env={'RVC_SOUND': '1'}`, `image=`, `race_at=`, `race_until=`. Set `workers=True` when it uses workers. Keep it to a minute of the guest's; tag `long` otherwise. Give the source of each value in a comment. A game's sums also go in its doc's "Checking it".

## On the card

    python tools\test_card.py mctest-3 fptest               # through gpu_queue, one at a time
    python tools\test_card.py machine --list                # chosen, and why one is skipped
    python tools\test_card.py mctest-255 cc-bytes --parts   # runs of --part-secs, the machine saved between
    python tools\test_card.py mctest-3 --warp --parts --max-wall 240 --mark-ok

- It resumes a shell snapshot (`build/test_card/shell.snap`). After a layout change or a new image: `--snapshot`.
- A case is skipped for another image, for host words that change as it runs, for `rvc_cpu`'s own devices, or for being too long without `--parts`. "11 of 11, 4 skipped" is a full pass if the four are the known ones: say so.
- Card values are in `CARD` (`tools/test_card.py`), each the same in two runs. Float programs differ from `rvc_cpu`'s by design.
- A tick not stamped exits 3: `tick-and-shader-changes`.
- Leave the card alone while VRChat or SteamVR runs, and if `rvc_harness.exe` with no arguments is up.

## Report

The tool's own summary line as printed, the image tested, `--races`' undone, and each skip or failure with its cause. A case not run is not a pass.
