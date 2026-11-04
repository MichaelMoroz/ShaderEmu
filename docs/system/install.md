# Installers unpacked in the machine (`cabinst`)

Two games' data may be passed on only as their maker's whole, unchanged package with its licence: Microsoft's 1997
trial of Age of Empires (`MSAoE.exe`, its `eula.txt`: "a true and complete copy ... accompanied by a copy of this
EULA ... not for commercial purposes") and LucasArts' 2002 demo of Jedi Outcast (`jk2demo.exe`, its `license.txt`:
copies "only in its original form", no files added, changed or deleted). So since 2 November 2026 an image made with
either game holds the **installer itself, byte for byte**, in the ROM, and nothing taken out of it; the machine shows
the licence read out of the installer, and once it is accepted unpacks the files the game reads into memory, every
session. `linux/aoe/data.py` and `linux/jk2/data.py` put the installer in; the games stay out of `ALL` and `linux/prebuilt`, and go into every image
(`image.py`, since 4 November 2026; `AOE_IMAGE=0` and `JK2_IMAGE=0` make a private test image without them).

    cabinst [--title TEXT] [--licence NAME] [--key KEY] INSTALLER DEST WANTED...

`WANTED` is `NAME[=PATH]` (a file of the cabinet, `/` or `\` between folders, any case), `*` or `*.ext`. The starters
(`aoe`, `jk2`) call it first; `aoe --unpack` and `jk2 --unpack` do only that.

| | Age of Empires | Jedi Outcast |
|---|---|---|
| The installer | 24,786,864 bytes: a program, then a Microsoft cabinet at 55,136 (129 files, one MSZip folder of 1,437 blocks, 47,069,189 bytes) | 69,680,101 bytes: a program, then a cabinet at 168,600 (45 files, one folder of 2,266 blocks, 74,230,162 bytes) |
| Its licence | `eula.txt`, 5,065 bytes | `Disk1\Install\license.txt`, 3,529 bytes |
| Unpacked into `/tmp` | 105 files, 34,085,752 bytes (`*.drs`, the AI, maps, songs, `Empires.dat`, `language.dll`, the six faces) | `Disk1\GameData\demo\assets0.pk3` as `/tmp/jk2/base/assets0.pk3`, 64,001,989 bytes |
| The registry's key | `aoe.eula` | `jk2.eula` |

## The licence

The licence file lies in the folder's first megabytes, so its blocks are unpacked first (0.87 and 0.89 s on the card),
and shown whole in a window of its own (`lic_ask_window`): the text wrapped to the box, a scroll bar, the wheel, the
keys (arrows, Page Up and Down, Home, End), and **Accept greyed and refused until the text has been scrolled to its
end** ("Read the licence to its end (scroll down) to accept it."); Decline, Escape or the frame's close is 1, and the
starter ends. Without a window system the text is printed on the terminal and `yes` typed accepts it. An acceptance is
the registry's key (`registry.h`) set to the licence's CRC-32: while `/persistence` is kept the window is not shown
again for the same text, but the files are unpacked at every start (they are in memory; `/persistence` holds 96,000
bytes). `DEST/.unpacked` says the files are there. The starters delete `DEST` when the game ends, however it ends (a
`trap` on the shell's exit; the game is not `exec`ed): 34 and 64 MB of RAM are not left behind for the next program
(4 November 2026). `aoe --unpack` and `jk2 --unpack` (a test's) unpack and end, leaving the files for the run after.

## Unpacking on the workers

`linux/apps/cab.h` reads the cabinet where it lies: a ROM file mapped is the ROM itself, so nothing is copied to read
it. Its header is found after the program (the first `MSCF` whose sizes hold), then the folders, the files and each
block's place. Each round of 256 blocks (`CABINST_BATCH`) is three loops over the workers:

1. **The cabinet's own sums** of the round's blocks (its CFDATA checksums: every block of both installers has one).
2. **`media_mszip`** (libmedia, `docs/system/media.md`, "A cabinet's blocks"): every block on a core.
3. **The files' pieces**, 16 KB of a file each on whole texels of it, copied into the files' memory.

The files' memory is the files themselves: each is truncated to its size on tmpfs and mapped shared, and the workers
write into it in place. A write would end a pass for every page (a page copy is the machine's parallel copy), 16,384
passes for the pak. tmpfs without the MMU gives a file truncated up from nothing one run of pages so that it can be
mapped, and the kernel took one buddy block for it, 4 MB at most: `linux/nommu/patch_ramfs.py` takes the first run of
free pages long enough (`alloc_pages_run`) when the file is larger or no such block is free. On an older kernel a file
that cannot be mapped is written by the first core (`pwrite`), slowly.

`CABINST_PRINT=1` prints the licence's lines and where the buttons are, each scroll, the answer and each round;
`CABINST_SUMS=1` a CRC-32 of each file; `CABINST_WORKERS=N` the most workers (255; 0 none).

**Checked**: `linux/libmedia/test/cab_test.c` (`cab_test-host`, built with libmedia) unpacks every file of both
installers on the build computer, threads for workers, at 8, 0 and 3 threads and in rounds of 7 and 33 blocks: 129
and 45 files, every one the same size and CRC-32 as 7-Zip's extraction, and every block's sum as the cabinet says. In
the machine the pak's CRC-32 is 7-Zip's, `239abc19` (`test_all.py jk2-install`).

## What it costs

On the card (`rvc_harness` through `tools\gpu_queue.ps1`, 2 November 2026), with 255 workers:

| | Age of Empires | Jedi Outcast |
|---|---|---|
| The licence read out | 0.87 s | 0.89 s |
| The files unpacked | 6.25 s (all 47.1 MB of the folder inflated) | 6.69 s (74.2 MB inflated) |
| `aoeprep` (the strings, the faces) | 0.26 s and 4.30 s | |
| The starter's `--unpack`, from the command to its end | 16.6 s | 12.3 s |

Without workers, on the card: Jedi Outcast's pak 439.4 s (2.18 billion instructions on the first core, 4.9 million a
second). Age of Empires was not timed so on the card (it would hold it for some 15 minutes); `rvc_cpu` with the card's
model (`--pass-model` of `tools/card_model.json`, `RVC_WCACHE=1`): Jedi Outcast 6.04 s with workers and 398.4 s
without, Age of Empires 6.49 s and 546.7 s, `aoeprep`'s faces 2.85 s and 215.7 s. Instructions a byte inflated, on
the first core alone (`rvc_cpu`): the demo's folder 29.2 (2.17 billion for 74.2 MB; on the workers 2.15 billion), the
trial's 64.5 (3.04 billion for 47.1 MB: more copies from the block before, followed back). See the games' docs for the
memory in play.

What a round costs: every block is a core's, so a round lasts as long as its slowest block, and a block that copies from
the one before it must be inflated as values (a byte, or the place in the block before) and then followed back. In
the trial's folder 1,417 of 1,437 blocks did that and 43% of its bytes were such marks, 7.2 blocks back on average;
following them was 8.5 of 12.5 s (the card's model) until a mark followed is written back as the byte it came to (a later block's chain
ends there from the next pass on): 2.5 s. The demo's pak is mostly deflated already: 954 of 2,266 blocks have marks.
