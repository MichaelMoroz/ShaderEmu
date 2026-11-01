# Play and other people's PNGs, 1 November 2026

The goal: +25% against the tree before the recent work, measured on the card, with passes not ending full noticeably more often or much sooner. Raw speed only. "Measured" is the card through `tools\gpu_queue.ps1`, alternating with the tree (`build/images/nommu` of the main checkout), three runs a side, unless it says otherwise.

| case | before the recent work | the tree | now | against before | +25% |
|---|---|---|---|---|---|
| Tiberian Dawn's play (`tdawn_play`) | 19,626 ms | 17,080 ms | **16,846 ms** | 1.165x | **not reached**, 1,146 ms short of 15,700 |
| png, no sum (`png_ns`) | 2,661 ms | 2,574 ms | **1,616 ms** | 1.647x | **reached**; full passes rose, see below |

## Tiberian Dawn's play

Measured:

| | wall ms | passes | core 0 M (card) | budget | pause | full |
|---|---|---|---|---|---|---|
| tree | 17,080 17,071 17,091 | 11,633 11,612 11,613 | 88.2 | 82.8% | 17.0%, 4,672 a pass | 0.2% |
| q4 | 16,861 16,835 16,844 | 11,301 11,277 11,282 | 87.8 | 88.4% | 11.5%, 4,620 a pass | 0.1% |

`rvc_cpu`, core 0 in play: 87.59 M to 87.42 M. State `1a9a814b`.

**What bounds it (first question of the brief).** Core 0. Before: 82.8% of passes ended by the budget at 1.52 ms, 1,979 passes paused at 4,672 instructions and 1.21 ms. Were every wait gone, 10,772 passes x 1.52 = 16,373 ms, about 700 ms of the 1,370 asked for (inferred). A paused pass is not a pass lost: it cost 0.36 ms against its instructions in a full pass. After this work 1,287 are left: at most about 460 ms (inferred), of which 813 passes are `mcw_rest` (the sort's and the scene's joins, the path search's) and 474 `host_settle`.

So 15,700 wants at least 3.8 M more of core 0 (at the slope of 180 ms a million) on top of every wait gone, and about 6.3 M with the waits as they are. I found no such thing. **Stopped there.**

What was changed:

- **The palette waits, not the game** (`host_palette_set`): `Color_Cycle` right after a show settled 524 passes a run. Costs 0.2 M instructions in itself.
- **The sort begins before the input** and any input drops it. Its share of the fall from 1,979 paused passes to 1,287 was not separated from the palette's.
- **A word of two empty lines is passed by before any mask** in `Greatest_Threat`. Its share of core 0 was not measured apart: with counts made again the whole is -0.17 M on `rvc_cpu`, the palette's own +0.2 M in it.
- Counts made again by `counts.ps1`, in the private home; a clean build at another path gives the same stripped bytes (`home2.sh`, `b31878f0`).

Tried and lost:

- **The box of `Greatest_Threat` read by rows** (hits sorted into the rings' order, stopping at the game's two breaking points, a crowd over 24 going back to rings): `rvc_cpu` core 0 94.08 M against 87.59 M, the function 6.07 M to 13.2 M, state the same. The rings stop early; the rows read all of a large box. Taken out.
- **`Behind_Note` on a worker** (4.5 M, 3.3 k a frame), not built: the rest of the frame's input and drawing is about 3 k a frame, so the first core would wait about a pass and a half for 3.3 k saved (inferred).

Left, by profile (core 0, `rvc_cpu`, tree): `InfantryClass::AI` 21.6 M with what is inlined, of which `Greatest_Threat` 10.0 M; `Behind_Note` 4.5 M; `memcpy` 3.2 M, 2.15 M of it the objects' copies for the drawing behind; `LogicClass::AI` itself 2.6 M. These are the game's order: each `AI()` reads what the one before stored, and the state sum holds it to that order. The likely way to 25% is a worker that scans for threats ahead with a check of what it read, on the pattern of the path searches. I did not try it: a scan reads other objects' places and states, which the same frame's logic changes, so a check is most of the work.

A budget pass is 1.30 ms at its 5th percentile and 1.77 at its 95th (median 1.52; `passtime.py`), while png's is 1.31. A line on instructions and workers does not explain it (15 us a running worker). What the rest goes by (what core 0 reads and stores, presumably) was not found. That 0.2 ms a pass is 2 s of play, more than this whole goal, and it is where I would look next.

Checks on q4: `test_all.py tdawn ralert` 19 of 19, `--races` 19 of 19. `TDAWN_CHECK_SCANS=1` 1,275, 0 wrong; `TDAWN_CHECK_SORT=1` 1,300, 0 wrong; `TDAWN_CHECK_PATHS=1` 0 wrong; 278 edge walks, 0 wrong. `RVC_RACES` with `RVC_RACES_NEAR=4` and `RVC_WCACHE=1` on the play: 0 undone, 0 stops. `hold.ps1`: `TDAWN_HOLD_LATE=600` with `TDAWN_SWAP=wait` and without, `--cpu --warp`, 0 pixels differ. Red Alert was rebuilt (it shares `host.c`) but its speed not measured. No save and load was played, and no game with real input ran (the drop path is reached only by input).

## PNG in one stream

Measured, `png_ns` on the fox:

| | wall ms | passes | core 0 M | full |
|---|---|---|---|---|
| tree | 2,573.5 2,573.9 2,574.0 | 1,977 1,977 1,978 | 15.97 | 0.6%, 12 passes, 5,490 a pass |
| p6 | 1,618.8 1,615.2 1,615.4 | 1,295 1,294 1,293 | 5.72 | 2.5%, 33 passes, 4,600 a pass |

**The constraint is not met as written:** passes ended full 12 to 33, and sooner. 22 of the 33 are in the kernel's page allocator (pc 82084070, 8207b300), for about 5.7 MB of values; lost 0.12 M of 5.7 M core-0 instructions (2%), against a wall 37% lower. Half the parts find no block and never use their rings: a first phase that finds the blocks, with rings mapped only for those, would take most of them away. Not built.

The images on the way, single runs: 2,572 ms the tree, then p3 1,984, p4 2,105, p5 1,637, p6 1,616. (p2's timing was spoiled by another run on the card, 2.8 ms a pass: disregarded.)

Where p6's passes go: program start about 100, the IDATs' join 50, parts 600, windows and values 250, bands 185.

Checks on p6: `check_pictures.py` 0 bad; `inflate_check.py` gz and png 0 bad; `RVC_RACES` with `RVC_RACES_NEAR=4` 0 undone on the fox at 63 and 16 workers, 3 and 4 channels, `MT_BUSY=20`, `nxview`, `MEDIA_PNG_ONE=1`; sums `53b5b44c` (3 channels) and `2f4476b2` (4), the same by the way back; `test_all.py media unarc nxview` 18 of 18. Only the fox was timed: other people's pictures went through `check_pictures.py` for their bytes, not for their time.

**Not run:** the other programs relinked with this libmedia (`nxweb`, `nxpaint` and the rest are the main image's), `linux/tcc/build.sh` and `cc-bytes`, the whole `test_all.py` and `--races`, `app_fuzz.py`, Unity. The prebuilt was not made again.

## The tools here

`home.sh`, `env.sh`, `mkimg.ps1` (a private home and image), `vc.sh` (the game's tree), `counts.ps1`, `home2.sh`, `flat_ra.sh`; `play.ps1`, `png.ps1` (the card, in turn); `frames.py`, `waits.py`, `timeline.py`, `bypc.py`, `who.py`, `passtime.py`, `passlog.ps1`, `passlog2.ps1`, `a2l.sh`, `adiff.py` (reading them); `inflate_check.py`, `png_check.ps1`, `hold.ps1` (checks); `card.jsonl`.
