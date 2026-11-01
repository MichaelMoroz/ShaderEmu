# A faster ISA by translating RISC-V: what was built and measured (1 November 2026, RTX 5070 Laptop, D3D11)

Every number below is the card with no other run beside it unless marked. Logs: `alt2.log` (ticks alternating in one run), `ab.log` (A B A B whole runs), `shapes.log`,
`classes.log`, `h2h/results.jsonl` (`h2h/show.py`). Snapshots and code files lie outside the repository (`C:\Users\micha\agents_isa`), made by `xlate3.py` from the snapshots' profiles.

## Against gpu_vm, today (`h2h/h2h.py`, cold boot of each machine, its own compiler, same programs; tick ms between marks)

| Load | Flat | World (11.1 ms, 1.4 ms, a round) | VR (11.1 ms, 7.1 ms, 1) |
|---|---|---|---|
| shell 4,000 turns | rv 13.2-14.0 s, gvm 10.6-11.2 s: gvm 1.25x | 13.6 / 10.8: 1.26x | 14.2 / 12.5: 1.14x |
| seq 1..200000 | 70.8 / 57.5: 1.23x | 73.0 / 57.6: 1.27x | 77.2 / 66.8: 1.16x |
| gzip twice | 13.3 / 8.2: 1.62x | 13.7 / 8.3: 1.65x | 14.2 / 9.5: 1.51x |
| Doom timedemo, realtics | 1904-1952 / 1379-1428: gvm 1.35-1.42x | 2309 / 3023: rv 1.31x | 6820 / 10350: rv 1.52x |
| Quake timedemo, s | 9.2-9.4 / 8.7-9.2: gvm up to 1.08x | 12.9 / 12.9: level | 30.3 / 53.4: rv 1.76x |
| nxray, 255 workers, ms a picture | 53-69 / 22-25: gvm 2.3x or more | 155 / 22 | 145 / 66: gvm 2.2x |

gpu_vm's Doom: 231 million bundles against 292 million RV instructions (21% fewer) at 196 ns against 213, and 126 thousand passes against 50 thousand, about 1,830 bundles a pass.

## The loops, whole runs against the tree (`ab.log`)

`wide3.h` and `xlate3.py`: each block list-scheduled, three ops a bundle, memory ops first (two at most), both successors fetched, tiles of 16 x 16 for the code, no x0 or unused reads, restart at every bundle
from a table (`--check` runs each block both ways on random registers, from every restart; the guests ran with no oops). Budget in RV instructions, so a pass is the same work.

| Load | Tree | Best translated | | RV a bundle | Write cache full, tree / translated |
|---|---|---|---|---|---|
| shell | 5.25 M/s | 4.74 (`w3x`) | -10% | 2.04 | 0 / 0 |
| Doom | 5.07 M/s | 4.76 (`w3x`) | -6% | 2.04 | 3.9% / 3.8-4.1% of passes |
| Quake | 1.84 M/s on core 0 | 1.74 (`w3y`, floats as ops) | -6%, drawn 14 tenths against 17 | 2.00 | none of either; 80% of passes end at the program's pause |
| nxray | 628 M/s all cores | 614 (`w3y`) | -2%; same instructions | 1.86 | 1 pass of each |

## What each tried form did (ms a pass of 8192 RV on shell / Doom; tree 1.41 / 1.46)

| Form | shell | Doom | |
|---|---|---|---|
| Earlier `wide.h` as committed: no reordering | | | -13% / -14% whole |
| `wide.h` in gpu_vm's form: both successors, six reads first, selects (`WIDE_F`) | 2.73 at 12,740 RV = 214 ns an RV | | level with its own branching form (2.73 / 2.32) |
| Reordered, one op a bundle, no chains | 2.45 | 2.38 | 1.24 RV a bundle |
| Three ops, one memory | 1.87 | 1.90 | 1.84 |
| Three ops, two memory, one fetch (`w3`) | 1.86 | 1.84 | 2.04 |
| The same, both successors fetched (`w3f`) | 1.72 | 1.68 | fetching both is cheaper than branching to one |
| Old step out of the loop (`w3a`) | 1.69 | | 2% |
| One memory op a bundle, then only one memory path in the shader (`w3b`) | 1.74 at 1.84 RV | | 7% a bundle |
| Old loop gone, general step for what is left (`w3c`), floats out too (`w3d`) | 1.84, 1.81 | | worse: leaves cost more than the shader's size |
| Hot layout | 1.73, 1.67 with tiles | 1.71, 1.66 with tiles | no gain |
| Code in tiles of 16 x 16 (`w3t`) | 1.63 | 1.62 | 3 to 4% |
| An empty op not read or written (`w3u`), control's registers only with control (`w3v`) | 1.57, 1.58 | 1.56, 1.56 | 3%, the second nothing |
| x0 and a load's data not read (`w3x`) | 1.59-1.62 | 1.57-1.58 | level |
| x8 to x15 in shader temporaries as well as the array, a read by selects (`w3h`) | 1.79 | 1.78 | 13% worse |

`w3x` by shape on Doom, 1.22 to 2.04 RV a bundle: 1.94, 1.73, 1.70, 1.57 ms. A straight line: 135 ns a bundle and 1.03 ms a pass of what no bundling takes away (126 ns an RV instruction; the tree's whole
instruction is 178). So at any density the loop has 1.03 ms; +25% is 1.17 ms, which leaves 0.14 ms for 1,000 bundles: 8 RV a bundle. `pack2.py`'s ceilings with reordering: 2.7-3.1 at four to six ops, 3.3-3.9 at eight
(inferred: at 3 RV a bundle, if a bundle stayed at 135 ns, 1.03 + 0.37 = 1.40 ms, +4%; a second texel a trip is not paid for there).

## Prices

Tree (`classes.ps1`, cached tight loops): ALU 143 ns, load 174, store 190, two-instruction loop 146. gpu_vm's own (`--case`): add 55 ns (50 an RV instruction), lw 129, sw 167. So memory is within 1.2 to 1.35x, arithmetic is 2.9x: yet
in real code a bundle of gpu_vm's is 196 ns (Doom) and this loop's empty trip is 135.

`promo.py`, what no translation of a block sees but a promotion could: saves of callee-saved registers 5% of instructions, restores 5%: at most about 7% more if none of it were needed (inferred).

## Not built

- Promoting stack slots to more registers across calls: needs each slot proved unread by anything else, a signal or a context switch's view of the stack; 7% at most by the counts above.
- Two memory ops from one texel at one read (26-36% of memory ops are in the last one's texel): not separately built; two memory paths cost what `w3b` shows and came out worth it by density alone.
- memcpy and loop ops: the machine's copies already go by the GPU; a loop's own form was not built.
- Two texels a bundle: not built, see the line above.
- The 8 x 8 core: gpu_vm's own RESULTS has its levels; here the pass already holds at 8192.
- `tcc`, `hlsl`, the NES, `rvc_cpu`: nothing passed, so nothing brought in.
