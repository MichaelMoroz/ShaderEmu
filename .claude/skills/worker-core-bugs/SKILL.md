---
name: worker-core-bugs
description: Find and fix bugs of the ShaderX86 machine's worker cores - stores lost at the commit, two variables in one 16-byte texel, code a worker reaches that stores outside its core's state, a worker stopped at a page, a game right on rvc_cpu and wrong on the card now and then. Covers RVC_RACES, RVC_RACES_NEAR, RVC_RACES_AT and _UNTIL, RVC_BESIDE, RVC_WCACHE, the [cpu] stops line, tools/mc_candidates.py, naming a racing pc, per-core state and how to tell a real fault from a harmless one. Use when RVC_RACES says undone, when a program with workers gives different sums, freezes, or fails only on the card, or before calling worker work finished. Triggers on: RVC_RACES, undone, near, as it was, texel, race, worker hang, stops, mcw_on_worker, SE_Who, test_all --races, wrong on the card now and then.
---

# Worker-core bugs

Depth: `docs/machine/multicore.md` ("Using the workers from a program", "Races", "Passes apart"). General crash reading: `debugging-the-machine`.

## The model

On the card every core reads RAM as the pass began, and a store is whole 16-byte texels: the highest core's 16 bytes win at the commit. On plain `rvc_cpu` cores run in turn and see each other at once, so a program right there may be wrong on the card, now and then, in only one link's layout.

## 1. Run the check

    $env:RVC_THREADS='8'; $env:RVC_RACES='logs\races.txt'; $env:RVC_RACES_NEAR='4'
    bin\rvc_cpu.exe --quiet --uart-log logs\r.log --wall 120 --expect "/ # " --send "nano-X -p & sleep 2; CMD; echo LX-''DONE\n" --until LX-DONE

Checks the tool: `mctest 3 0 race` must report 49 undone at one place with `0 0 49 0` on the console, and `mctest 3 0 near` `50 0 50 0` with 99 changes 1 pass apart, 0 undone (run 31 October 2026).

Add `RVC_RACES_AT=TEXT` and `RVC_RACES_UNTIL=TEXT` to check a part of a long program (`[races] not checked` means the text never came: that is no pass). `python tools/test_all.py --races` runs every case so, and prints near counts beside PASS (`NAME.log.races` in the logs). `python tools/game_par.py` sets `RVC_RACES` by default.

## 2. Read the output

    [races] 49 changes undone by a higher core's to the same 16 bytes ...
    [races]   undone x 49: core 2 at pc 82701be0, core 1 at pc 82701be0, e.g. 8271c158
    [races]   near x 99: core 2 at pc 82701c20 (words 4), 1 passes after core 1 at pc 82701c20 (words 1), e.g. 8271c150

| Line | Meaning | What to do |
|---|---|---|
| `undone` | a lower core's change is lost, as at the commit | **Always a fault.** Must be 0 |
| `as it was` | a core read another's change from the same pass | Fine for a core waiting on a word, a pass late. A fault only if the program assumed it fresh: its own sums say |
| `near` | a texel changed by two cores within N passes, other words | A report: a hand-over by a job word is one, and every game has thousands. Look at those between statics that are not a hand-over: on the card a pass ends early, and they fall into one pass |
| `[cpu] stops: workers' faults N` | a worker stopped at a page until core 0 served it | Must be 0 in steady play: touch first |
| `[beside] ... texels stored by workers in the pass core 0 parked them` | a store landing in memory the program may no longer have | look at `mcw_close` timing |

## 3. Name it

    python tools\mc_candidates.py CALLS ELF --races logs\races.txt       (CALLS from RVC_CALLS=FILE)

Or by hand, `pc` to function (`debugging-the-machine`, "pc to source"), and the example address `e.g.` to a variable: `riscv32-linux-nm -n ELF`, address minus the base. Two names in one 16 bytes is the commonest answer: `world_ahead` beside `iTouchLinkSemaphore`, `serving` beside `se_asks`, `line_news` beside `matrices_dirty`, `mcw.c`'s own `page` and `count`, `_tw_tick_time` and `_tw_next`.

## 4. Fix by kind

**Two statics in a texel.** Give what a worker writes 16 bytes of its own: `aligned(16)` and padded (`SE_APART`, Doom's `APART`), or compile it apart: `-fno-common -fdata-sections` with `objcopy --set-section-alignment` and a `zz_pad` object last. Under whole-program LTO a library's statics lie by chance among the program's: the link needs those flags too, or a section assembler (`ottd-as`). A fix by moving one variable is luck unless the rule is built in.

**A per-core array of less than 16 bytes a core** (OpenTTD's `WaterRegionCheck`, 12-byte vectors). 16 bytes a core.

**A shared word written by every share** (Doom's "settled" word: 29,685 undone). One share clears it and the others wait for that (`drawn_wait`).

**State both sides write.** A copy a core: `x_of[3]` behind `SE_Who()`, `SE_RenderWho()`, `SE_OnWorker()`, `VpCore()`, `ShaderEmu_Core[who]`. Then make it on every core that uses it: Half-Life's hit hulls existed on core 0 only, and nothing crashed; bullets went through monsters. Found by playing, so play.

**Library state.** `malloc`, stdio, a file open: core 0's alone while core 0 uses them. Use `mcw_print`, `mcw_defer`, `mcw_scratch`, `mcw_fail`, not code of your own.

**A worker that needed core 0.** A level frozen in its first corridor came from a sequence group first played on the drawing's worker. Half-Life's server and drawing run on stacks where `mcw_on_worker()` is false: the code's "am I a worker" lies. `FS_OpenInflated` goes through `SE_ServerOnCore0`; load the group with the model.

**Walking a pool core 0 moves.** `Mod_CacheCheck` off core 0 goes by the pointer. A worker reading a list another core rebuilds is a crash with no `undone`.

**A flag set before its data.** Store after `mc_publish()`: a pass ends at any instruction and the compiler may reorder.

**Stale caches over two sets of copies.** With a frame behind, a cache keyed by a copy's address breaks: go by the live entity (`R_SceneLive`).

**Pages.** A store by core 0 first to memory a worker will store to, GPU memory and mailboxes too; code read first (`mcw_touch_code` from `_start`'s page, as `__executable_start` is 0 at run time). `PHYS_COUNT=1` and mctest's "pages brought for their start" must say 0.

**A waiting job.** `mc_end_pass()` between looks, never the pause hint (a worker's sleep) and never a spin. First core: `mcw_rest()`, and it polls helpers (`helpers_poll`).

**Exit.** A worker reads its start word as its pass begins, so one in that pass still stores. `patch_exitpark.py` parks before `exit_mm`; test `killall -9` mid-run, then `mctest 3 120000` must PASS.

## 5. Harder settings

| Setting | What it adds |
|---|---|
| `RVC_BESIDE=1` | core 0 beside the workers as on the card, not before them; its fills and `fence.i` as on the card |
| `RVC_WCACHE=1` | every core's write cache: passes end where they end on the card |
| `RVC_RACES_NEAR=4` | stores a pass or two apart, which `RVC_RACES` alone will never see (found Half-Life's `net_drop`) |
| `--threads 1` | the same instructions and console every run: for a repeatable bug |
| `RVC_FAST_CHECK=1` | fast handlers against the general step, with `RVC_WCACHE` too, not with `RVC_RACES` |
| `--max-passes N --save F`, then `--load F` | go back to just before it; `RVC_RACES`' tables are not kept, each call reports its own part |

`RVC_ADFAULT` changes nothing on the no-MMU kernel: there is no walk.

## 6. Done means

- 0 undone with and without the way back (`HL_RENDER=core`, `NAME_X=inline`), and sums equal between them.
- The play that was changed, walked (`optimizing-games`, section 8). Level changes and closing it too.
- `python tools/test_all.py --races`.
- `python tools\test_card.py CASE` for what only the card can show. A race fixed by luck comes back in another link's layout: the near list is how to tell.
