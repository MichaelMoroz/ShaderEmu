# The memory model: what a pass stores, and what would let it store more (23 October 2026)

Plan item 3 of `HANDOFF.md`. The question: would a memory model the compiler manages (a core-private scratch or stack
window kept out of the write cache, explicit cache operations, coalesced stores) cut the distinct texels a pass stores?
Half-Life ends 84% of its passes on a full write cache on the card (x23, about 4,000 bundles a pass). The constraint
stays the same: the tick is a fragment shader with no UAVs, so a pass's stores reach RAM only through the core's state,
which the commit draws.

**The answer is no.** The stack is a sixth of a pass's distinct stored texels, and keeping it out of the cache gains
5%. Two things limit a pass: the cache's capacity (4 KB) and how full x23's buckets get before one overflows (two
thirds full). Both are the tick's to fix, not the compiler's.

## Measured: Half-Life on gvm_cpu, from `hlmark: begins`

`GVM_STLOG=FILE` (gvm_cpu) logs core 0's stores between two console lines: the address, sp, the bundle count, the base
register, user or kernel, and the pc. `GVM_STLOG_LOADS=1` logs the loads too. `gpu_vm/cpu/stlog.py` reads the log and
plays x23's cache over it, two tables of 32 buckets of four texels with x23's hashes. The run was `hldemo1`,
`HL_MARK=31,43`, 32 cores, 3 to 4 M stores logged after the mark (about 10 M of core 0's bundles). The program's data
and bss came from its ELF (`halflife.gdb`: runtime = ELF address + 0x4160040).

Distinct texels stored in a 4,096-bundle window (about 200 in all, 3.2 KB):

| | texels | share |
|---|---|---|
| user globals (data and bss) | ~98-115 | 50-57% |
| user heap and other | ~38-48 | 20-25% |
| user stack (sp or fp based, or within 32 KB above sp) | ~28-40 | 16-20% |
| the kernel (its stack 5, the rest 1-2) | ~7 | 3% |

- 75% of the stored texels get all four words written (a mean of 3.5 words a texel; the heap's 3.0). Each entry is
  already one texel holding a mask of its words, so coalescing stores has nothing left to gain.
- By function, **memcpy accounts for 42% of a pass's new texels**: 84 texels a window, 76% of them into globals. These
  are struct copies, mostly 64 B to 1 KB (8% of copied bytes are over 4 KB), into arrays such as the entity states.
  After memcpy come CL_CopyEntityToPhysEnt 6%, se_ref_guard 4%, CL_EmitEntities 4%, the PM_ traces' stack 4%, and
  memset 3%.
- Stack stores make up 42% of all stores but only 16-20% of the distinct texels: the same frames are written over and
  over and hit in the cache.
- At its first overflow, x23 holds about 170 texels of its 256. Two-choice buckets of four ways fill to about two
  thirds before a pair is full. Other hashes (multiplicative, wider folds) did not help: 156-169 texels.

**Quake III** on gvm_cpu does not reproduce the card's behaviour: core 0 is in `REST` most of each frame, and 87% of
its passes end on sync with 17 distinct texels a pass. Its store mix is the card's question, not this one's.

## Options, played over the same Half-Life store log (bundles a core 0 pass, x23's model)

| | bundles a pass | |
|---|---|---|
| x23 as it is | 3,620 | 72% of passes end full |
| + a stack window of 4 KB (spills and frames about sp kept in the core's own state, out of the cache) | 3,800-3,820 | +5% |
| memcpy of 1 KB or more as the commit's copy (non-blocking; an access to its destination ends the pass) | 3,707 | +2% (512 B: -1%, the destinations are read in the same pass) |
| an insertion that finds both buckets full moves one resident to its other bucket (one step of cuckoo) | 4,249 | +17% |
| the same + 8 victim slots in place of 2 | 4,465 | +23% |
| a fully associative 256 | 4,453 | +23% |
| core 0's cache 512 texels (2 x 64 x 4), + the move | 5,939 | +64% |
| core 0's cache 1,024 texels (2 x 128 x 4), + the move | 6,781 | +87% |

Combining the stack window with the larger caches adds under 1%.

## Prototyped: the tick's cache in the reference, and gvm_cpu's passes ending like the card's

The reference (`gvm_ref.h`) now has a write cache model, off by default (`gvm_wlog.nb, ways, reloc`). When it is on,
a store whose texels the model cannot place ends the pass, as a full log does. `gvm_cpu --wc NB,WAYS[,RELOC]` turns it
on for core 0 and `--stats` prints why core 0's passes ended (counted from `--profile-at`'s line on). Half-Life
`hldemo1` from `hlmark: begins` to `hlmark: ends`, 12 s of the level at 0.2 ms a pass (a pass a frame, as in VRChat),
32 cores:

| core 0's cache | bundles a pass | passes ended full | HL frames in the 12 s |
|---|---|---|---|
| none (the reference's 3,072-word log) | 6,065 | 18% | 3,693 |
| x23 (`--wc 32,4`) | 2,986 | 91% | 1,846 |
| x23 + the move (`--wc 32,4,1`) | 3,576 | 81% | **2,235 (+21%)** |
| 512 + the move (`--wc 64,4,1`) | 5,193 | 46% | **3,210 (+74%)** |
| 1,024 + the move (`--wc 128,4,1`) | 6,109 | 20% | **3,736 (+102%)**, as with no limit |

The model ends more of Half-Life's passes than the card does: 2,986 bundles a pass and 91% full, against the card's
4,020 and 84%. The model is a proxy for the card, so read the ratios, not the absolute numbers.

## The design chosen

1. **Core 0 gets a write cache of its own, 1,024 texels.** Every pixel of a core runs the whole core, so core 0's
   extra pixels cost no time as long as its warps are its own (core 0 has a block of its own on the RISC-V machine,
   `docs/machine/multicore.md`). The cache is sized at compile time, so core 0 needs a separate compile of the step
   (`NB 128`) and a draw for core 0's rectangle; the workers keep x23's 256 in 6 columns. Its state is about 1,024 +
   256 tag texels + 40, or 165 pixels; the commit draws core 0's 1,026 points. The control pass and the harness
   read core 0's state by its own layout. Predicted: Half-Life about 2x the frames at one pass a frame, equal to an
   unlimited log.
2. **The move on insertion** (step 1 of a cuckoo, plus more victim slots), in the general step only. A store that
   finds no room leaves the fast loop: it does not end the pass. The general step moves one resident of the two full
   buckets to its other bucket (its tag and four words, a loop capped at 8), then the store goes on. The fast loop's
   text stays as it is, which matters because x20's cuckoo cache cost 5-29% a bundle. Predicted: +17-23% at x23's
   size, for workers too.
3. **The compiler: nothing for now.** A stack window brings 5%, and memcpy as the commit's copy 2% (memcpy's
   destinations are read in the same pass). Revisit once the cache is larger: with 1,024 entries, the passes that
   still end full are the large copies and the long frames.

Done since: items 1 and 2 as tick x25 (`gpu_vm/tick/steps/README.md`, "x25"; at a pass a frame Half-Life 2.2x x23's frames, as
predicted, but a pass costs 2.7x the card's time). Not done: Quake III's split measured on the card's own
workload. The proxy for core 0's speed on the card is bundles a pass and passes ended full with `--wc`, from
`hlmark: begins` (`build/memmodel/wc.sh` in this session).
