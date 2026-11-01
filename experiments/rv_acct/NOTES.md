# Where the real cases' time goes on the lean tick (1 November 2026)

The question: +25% on real cases, passes as long and as rarely ended full as lean's. Nobody had made a time account: this is it, and it chose the lever.

Flat, on the card (RTX 5070 Laptop), cold boot each, through `tools\gpu_queue.ps1`, the main checkout's `bin\` and image. Mostly one run each; where two runs exist they agree to 1 to 2%. **Measured** unless it says inferred.

| script | what |
|---|---|
| `acct.py`, `results.jsonl` | a load, its per-pass log read: passes by what ended them, by place |
| `split.py`, `show.py` | the tables below |
| `prof_all.py` | `tools/prof.py` on `rvc_cpu` for each load, `logs/prof_*.txt` (not kept) |
| `bootab.ps1`, `boot.jsonl`, `showboot.py`, `mk_img.py` | boot, two kernels in turn on one ROM |
| `gvm.jsonl` | gpu_vm on the sum-free loads, by `rv_vm/h2h.py` |

## The account as asked for

"Fixed" is an empty tick (0.036 ms) plus the run's own commit and device per pass; "early" is the extra passes from ending early. Shares of the wall. The split of core 0 and workers takes core 0's price from passes with no worker; where core 0 only rests, as in the sum-free rows, it says little, so read those by what ended the passes.

| load | wall ms | core 0 M | workers M | idle | fixed | early | core 0 | workers | instr a pass | ended full |
|---|---|---|---|---|---|---|---|---|---|---|
| boot, as `rv_vm` timed it | 1,606 | 7.8 | 0 | 17.4 | 9.6 | 0.6 | 72.4 | 0 | 3,871 | 1.3% |
| gzip | 15,169 | 90 | 0 | 0 | 13.0 | 0.1 | 86.9 | 0 | 8,132 | 0.3% |
| jpeg | 3,710 | 21.7 | 28 | 0 | 12.8 | 0.5 | 84.9 | 1.8 | 7,904 | 2.6% |
| png | 5,954 | 35.9 | 78 | 0 | 13.4 | 0.1 | 86.8 | ~0 | 8,139 | 0.2% |
| mp3, 63 workers | 4,128 | 19.5 | 222 | 0 | 10.4 | 3.9 | 68.5 | 17.3 | 5,956 | 3.6% |
| mp3, no workers | 47,970 | 239 | 0 | 0 | 10.9 | 0.6 | 88.5 | 0 | 7,734 | 10.7% |
| flac, 63 workers | 1,629 | 8.9 | 17 | 0 | 11.6 | 1.7 | 94 | ~0 | 7,169 | 1.6% |
| mpeg, 8 workers | 63,981 | 340 | 699 | 0 | 11.7 | 0.5 | 71.8 | 16.1 | 7,872 | 1.5% |
| avi, 16 workers | 31,026 | 172 | 350 | 0 | 12.2 | 2.2 | 71.7 | 13.9 | 6,957 | 0.2% |
| Tiberian Dawn | 28,444 | 144 | 122 | 0 | 11.2 | 0.9 | 85.7 | 2.3 | 7,588 | 4.2% |

## Two things in that table are not the load

**Most of the media rows are the test's own sum on core 0.** `NXVIEW_SUM`'s `fgetc` loop is 80% of the jpeg's core 0 and 48% of the png's; `media_test`'s `media_sum` and word loop are 94% of mpeg's and avi's core 0 and 53% of mp3's on workers; flac 13%. So `rv_vm`'s rows for these timed a checksum on core 0.

**Boot's idle is the harness typing.** Idle comes in runs of 35 ms after the prompt, while the test's command arrives. To the prompt (`RVC_MARK='/ # '`) a boot is about 1,307 ms with 20 to 57 ms idle.

## The media without the sums

`MEDIA_TEST_NOSUM=1`, no `NXVIEW_SUM`; mp3 and flac are cut at the last worker's pass (`--cut`). The video players ask for 24 workers (mpeg, `docs/apps/video.md`) and 64 (avi, its sums line); `media_test`'s 8 and 16 were only arguments.

| load | no sum, ms | core 0 M | workers M | instr a pass | ended full | what ends passes, bounds it |
|---|---|---|---|---|---|---|
| jpeg | 414 | 1.7 | 28 | 5,643 | 23.0% | 71 full passes, 18 to 20 each in `alloc_pages_run` and `free_page_series`; 10% of the wall |
| png | 2,661 | 16 | 78 | 8,079 | 0.6% | `inflate` on core 0, 92% of core 0; the workers spin for it, 60 M of their 78 M |
| mp3, 63 workers | 2,042 | 7.4 | 222 | 6,416 | 8.8% | one piece's length, inferred: 35 pieces |
| mp3, 255 workers, pieces of 5,760 frames | 1,046 | 3.0 | 246 | 4,548 | 10.0% | 268 passes of 653 end in a `memset` fill; same sum `fd612a25` |
| mp3, 63 workers, pieces of 11,520 | 2,174 | 7.5 | 228 | 6,393 | 9.2% | no gain: only 33 cores ran at once |
| flac, 63 workers | 1,361 | 7.1 | 17 | 7,481 | 1.6% | its decoding on core 0 before the pieces, 11% fills |
| mpeg, 8 workers | 28,178 | 67 | 699 | 4,001 | 3.7% | core 0 rests in 83% of passes, 6.6 workers a pass |
| mpeg, 24 workers | 12,258 | 49 | 699 | 6,151 | 8.1% | core 0 rests in 50%, 18.4 workers a pass |
| mpeg, 64 workers | 12,349 | 49 | 699 | 6,145 | 8.1% | no more than 24: the film's groups bound it |
| avi, 16 workers | 6,305 | 6.8 | 350 | 1,459 | 0.9% | core 0 rests in 95%, 11.7 workers a pass |
| avi, 64 workers | 2,303 | 4.5 | 350 | 2,858 | 3.5% | core 0 rests in 80%, 40.2 workers a pass |

A second of sound is about 13 to 14 million instructions, so the sound is far ahead of real time: this speed shows when a file is decoded whole (Half-Life's `snd_media.c`), not in `nxplay`. `media_test`'s piece size is its own argument: half a second stays libmedia's default, not changed here.

Instructions a pass here are core 0's: where core 0 rests they say nothing of the constraint.

**gpu_vm against lean on the same sum-free loads**, one run each (`gvm.jsonl`): `x` is lean's time over gpu_vm's.

| load | lean ms | full | gpu_vm tree ms | full | x |
|---|---|---|---|---|---|
| jpeg | 414 | 23.0% | 421 | 59.8% | 0.98 |
| png | 2,661 | 0.6% | 1,797 | 29.0% | 1.48 |
| mpeg, 24 | 12,258 | 8.1% | 9,974 | 27.9% | 1.23 |
| avi, 64 | 2,303 | 3.5% | 5,180 | 42.1% | 0.44 |

So `rv_vm`'s 1.15x on jpeg, 1.36x on mpeg and 1.03x on avi were largely of the sums: these are the figures. png's 1.48x is `inflate` on one core, as gzip's 1.71x is `deflate`; it breaks the constraint at 29% full.

## What each lever gave

**Boot: built, `linux/nommu/patch_bootwork.py` plus `CONFIG_CONSOLE_LOGLEVEL_DEFAULT=4`. 1.42x, no change to the tick.** In turn on one ROM, three runs each:

| | to the prompt, ms | instructions | passes | a pass | ended full |
|---|---|---|---|---|---|
| image's kernel | 1,291, 1,293, 1,337 | 7.2 M | 1,003 to 1,132 | 6,380 to 7,140 | 2.2 to 2.5%, 25 to 26 passes |
| patched | 908, 924, 926 | 4.7 M | 837 to 840 | 5,606 | 3.0 to 3.2%, 26 passes |

Instructions a pass fall because whole-budget passes went and the short ones stayed; the passes that end full are as many. Inferred from the profile, not measured apart: free ranges' page loops 1.4 M, console 0.4 M, uevents 0.4 M, IP ids 0.3 M. What goes: the kernel's notices on the console (`dmesg` keeps them).

The image's kernel and the patched one are not from one build: the first is the main checkout's image of 06:58. `python tools/test_all.py` on the patched image: 214 of 214 (all but the `lazy` image's case, which wants a build in the checkout; `--long` not run). Not in `linux/prebuilt` or the shared image.

**Video and bulk sound: worker count, no new code.** mpeg 8 to 24 is 2.30x, avi 16 to 64 is 2.74x; but the players already ask for those, so this corrects the baseline and gains nothing for a user. mp3 at 255 workers and pieces of 5,760 frames is 1.95x and is a real gain for a whole-file decode only; its 268 fills of a new buffer are the next thing there (a test's `aligned_alloc`, inferred).

**ISA: not reached by the numbers.** Zba, Zbb and Zbs are -2.7 to -3.8% of core 0's instructions (`docs/plans/riscv-isa.md`, not measured again), and a tick change costs 1 to 3%.

**Not built:**
- png: inflate is about 75% of its wall, on one core. Picture files made in segments (`png_segments.py`) already go on every core; for others a Huffman stage and a copy stage on two cores is the idea, inferred at most 1.3x, not tried.
- jpeg: its full passes are the kernel's allocator, 10% of the wall at most.
- The 225-entry step in Nsight: not looked at. gpu_vm's loss on avi, here without the large cache, says its workers are the first question.

## Tiberian Dawn

92% of core 0 is game code, 8% the kernel. `LogicClass::AI` and what it calls are 53% (`InfantryClass::AI` 27%, `Greatest_Threat` 7%, `BuildingClass::AI` 6.5%, `UnitClass::AI` 6.4%), `Render` 11.5%, the level's load about 28% of this window. Passes: 4.2% ended full, rest 3.6% and pause 3% of the wall. +25% wants about a fifth of core 0 gone; no single function holds it and the game's own workers have had many rounds already (`docs/games/tdawn.md`). **+25% not reached, no route tried here.** The load's 28% is one start, and frames at 60 a second are three times what the game wants.

## Whether +25% is reached under the constraint

| case | result | how |
|---|---|---|
| boot | yes, 1.42x, measured | kernel patch; full passes 26 either way, a pass 12% shorter by the mix |
| jpeg | no | 414 ms, 10% at most in its full passes, the rest workers |
| png | no | `inflate` on core 0; gpu_vm 1.48x breaks the constraint |
| mp3, whole file | yes for that use, 1.95x, measured once | 255 workers and smaller pieces; ended full 8.8 to 10.0% |
| mpeg, avi | no gain to a player; 2.30x and 2.74x only against the test's old arguments | |
| Tiberian Dawn | no | game logic on core 0 |

## Also found

`tools/tick_util.py` took a pc under 0xc0000000 for a program, so on the no-MMU kernel its idle, rest and kernel lines were all "pause: program". Fixed: the kernel goes by its own names' range.
