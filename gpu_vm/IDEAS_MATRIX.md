# The ideas that failed alone, and what each needs from another

Every idea of `IDEAS.md` that was rejected or not taken, by what it did alone, what killed it, and which other idea removes that cost (23 October 2026). AGENTS.md's last rule: optimizations are weighed together, and one that loses alone may win with another.

## The ideas

| # | Idea | Alone | What killed it | What the killer depends on |
|---|---|---|---|---|
| 3, 27 | Core 0's own large write cache (x25 1,026 entries; c0nb 194/258) | core 0 2.4x bundles a pass, full ends 17% to 10% | a pass 1.8-2.7x dearer, passes/s 4,930 to 3,300 | state I/O and clearing that grow with the entries; the pass costs what its busiest core costs |
| 22 | More buckets for everyone (nb 32, 64) | scenes faster | store-heavy work on many cores 0.65-0.84 | the same: local memory and state texels a core |
| 26 | Overflow entries, move to the other bucket (ov, mv) | passes 500-1,830 bundles longer | every bundle dearer: Quake III 69.8 to 57.7-67.4 | lookups read more tags on every load and store |
| 33, 39 | Silent stores: no entry for a store of what is there (sil 1/2/3) | Quake III workers' passes halved, fps as before | gvmbench 5-10% slower (path 6), Doom slower | a fetch and compare on the store path; **possibly the driver cliff** (40: identical-DXBC variants 15% slower) |
| 7 | A register file in the core's state (stack in it) | stack kernels +6-7% | 3-4% (128 words) to 12-16% (1024) on every bundle, unused | state I/O a pass; the file loaded whole at a pass's start |
| 14 | A read cache of the last 1-2 texels | loads 12-25% fewer | -6 / -10% overall | registers held across the loop; the texture cache already serves those |
| 19 | 128-bit SIMD registers | | 22.4 against 31.3 fps (ray) | a vector op is op 0 of path 1 only, and lanes cost vins/vext |
| 12, 17 | Late branches, fx1 (compiler packs more) | bundles -7.2% / -9.3% / -16.5% together | the tick +1.3-7% a bundle | the tick's branch reads its registers after the ops on every bundle |
| 28 | Callee-saved saves through a CSR | | Half-Life hangs at 4 | correctness, not cost |
| 29 | x30: blocks with their length and kinds read from the code | fixed shapes 1.27-1.31x in probes | variable blocks 94.8 against 77.6 ns | the shape varying at run time |
| 35, 36 | Strips: code and data in strips 512 wide | code past 16 KB 17-29% faster a bundle | small code 3-4% slower, games 1-3% slower | the address's ALU on every fetch's chain |
| 40 | Pairs: texel 2k+1 under 2k (the card's 32-byte sector) | cold code 20-34% faster | gvmbench small loops 3% slower | the same; `ubfe` in DXBC changes nothing (the driver fuses) |
| 40 | The lanes warm the cache at a pass's start (lpf, lps) | a cold chain's step halved (one core) | Half-Life -6% | 16 KB a core at every pass on up to 256 cores: paid by every core, every pass |
| 25, 41 | A longer budget a pass | | no change at 2K-32K | no pass reaches 3k bundles before the write cache is full |
| 25 | Fewer rounds of host work a pass (--tick-rounds) | | no gain | the card is the bound |
| 4 | Move on insertion | +17-23% in the model | within noise on the card | |
| 5 | Placement of busy workers | | bit-reversed -10% | |
| 32 | Half-Life's scene copy of changed words only | 13.7 to 13.0 | inside the data-placement swing (31) | layout noise |

## What one idea removes for another

| Combination | Why it could win where each lost |
|---|---|
| **silent stores + a smaller or equal cache + a longer budget** | 60-76% of Quake III's stores are silent: skipped, v0009's 130 entries end 12 full passes a frame instead of 33 (the replay), with no more state, clearing or tags. A longer budget is worth something only then (25, 41) |
| **silent stores, re-checked past the driver cliff** | 39's loss may be 40's cliff: build several equivalent variants (another knob's harmless value) and take the best of them as sil's number |
| **longer passes + the cold-cache fixes (pairs, lpf for core 0 alone)** | a restart's 110-150 us of cold cache is paid per pass; at 33 full ends fewer a frame it matters less, and a prefetch's cost is shared by more bundles. lpf on core 0 alone costs one core, not 256 |
| **strips or pairs + x30's fixed blocks** | the map's ALU is on every fetch's chain; with a block's length known, the address is made once a block and the next texels fetched together |
| **the register file + silent stores** | the stack is 16-20% of the distinct stored texels: kept in the file it makes no entries; with silent stores too the cache may hold a whole frame's work, so the file's 3-4% buys longer passes |
| **a big core 0 cache + the workers' budget against core 0 (wad)** | the big cache lost because the pass costs the busiest core; core 0 running longer while workers stop at its pace makes its pass the one that is long and worth it |
| **late branches + fx1 + the compiler's float-only mode** | -16.5% bundles against +1.3-2.9% a bundle at late=3: never timed together in the games |
| **a register file + callee-saved saves into it** | 28's saves to memory are stores the cache keeps; saved into the file they are not |

## Being tried

- The write cache (silent stores, more entries, RISC-V's shape, the driver cliff), judged on the games: an agent since 23 October 2026, 23:00.
- The cold cache at a pass's start: pairs and lpf, after the passes are longer.
