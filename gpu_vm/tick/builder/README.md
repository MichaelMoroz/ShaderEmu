# The tick builder (23 October 2026)

A tick (gpu_vm/tick/STATE.md's pixel shader) is no longer written by hand step by step: `gen.py` composes one from a point
of a parameter space, and `search.py` makes thousands of points, checks every one on WARP and times the ones that pass on
the card. x23's design is the default point.

    python gpu_vm/tick/builder/gen.py OUT.hlsl [name=value ...]          one variant (OUT.json: its manifest)
    python gpu_vm/tick/builder/search.py gen BATCH --n 300 --seed 1      random points (or --from BATCH:vNNNN --mutate K, --grid k=a,b)
    python gpu_vm/tick/builder/search.py check BATCH -j 8                 WARP, every variant, a timeout each
    python gpu_vm/tick/builder/search.py time BATCH --rounds 3            the card, through tools/gpu_queue.ps1
    python gpu_vm/tick/builder/search.py report BATCH                     scores against x23 in each chunk

A batch lives in `build/tick_gen/BATCH/` (vNNNN.hlsl, vNNNN.json, checks.json, chunk_*.txt, time_*.json and .log).

## Safety

- A built tick reaches the card only when `SHADER.warp_ok` holds the stamp of its source and defines: gvmbench writes it on
  WARP with `--mark-ok` when every check passed, and refuses on the card a built tick (one whose text says "made by
  gpu_vm/tick/builder") without it (exit 3). `search.py check` runs gvmbench's default set, `p_k1_m0`, `p_k4_m40` (idle
  and halted cores beside busy ones), `mc_int` at 40 cores (across bands at small `per`) and the phase 2 cases a tick has.
- gvmbench refuses a tick over D3D11's 4,096 temporary registers, on WARP too (so it never gets a stamp).
- Every loop of the tick is capped (run's guard, the fixed loops' bounds); the builder adds no unbounded loop.
- Every card run goes through `pwsh tools\gpu_queue.ps1`.

## The space (gen.py's DEFAULTS and SPACE)

| | values | what |
|---|---|---|
| nb | 16, 32, 64 | buckets of four ways a table (two tables, two overflow slots): 130, 258, 514 entries |
| bloom | 0, 1 | loads skip the tags when their texel has a bit no stored texel had |
| hash | 0, 1 | x23's xor-folds, or a multiplicative pair |
| clear | loop, unroll | the tags' clearing at a pass's start |
| fetch | both, one | the next bundle: both texels then a select (x23), or the branch first and one texel |
| int_first, mem_order, split | | the fast loop's test order, and branch or flatten at the integer/memory split |
| bw x bh, band, per | 8x8, 16x4, 4x16, 8x4, ... | a core's block, rows between bands, blocks a band |
| jrow | 0, 1 | pixel j column by column (x23) or row by row |
| perm, mul | id, bitrev, mul | which block of a band core c takes (bit-reversed, or c * mul mod per) |
| early | 0-3 | a halted (1) or sleeping (2) core copies its state at the top, without loading it; 3: and discards when both state sets hold the same (texel 23.w counts its quiet passes) |
| xr, npair, xbind, xload, xfast | phase 2 | the core's file (below) |

The layout is told to gvmbench by constants in the tick (GEO_BW, GEO_BH, GEO_BAND, GEO_PER, GEO_JROW, GEO_PERM, GEO_MUL;
CPTS the commit's points a core; XR, NPAIR, XBIND, XFAST); gvmbench draws the tick band by band (a whole band when its
blocks are permuted) and places the state the same way. The harness (`gvm_backend11.cpp`) and the control pass still
know x23's layout only: a winner with another layout needs them taught it before it runs a game.

## Phase 2: the core's file, bindings and pairs (isa.h, GVM_M_FREAD)

The ISA has (memory class modes 6 and 7; the reference `gvm_ref.h`, the assembler `gvm_asm.h`):

- `lf rd, ra, i` / `sf rs, ra, i`: a private file of XR words a core (GVM_SR_XR), slot (ra + i) mod XR, kept across passes in
  the core's state, no tags and no hash: the stack, spills and locals without the write cache.
- `fbind raddr, ra, i` / `funbind ra, i`: slot i bound to a RAM word (it reads the word); `sf` to a bound slot marks it dirty,
  and the commit writes every dirty bound slot at the pass's end (after the write cache, in slot order). bind and unbind of
  a dirty slot end the pass before the op.
- `fpair rs, ra, off`: an (address, value) pair appended to the pass's pair buffer (GVM_SR_NPAIR), written by the commit
  after the bound slots, in order; the bundle that fills the buffer ends the pass, and one that finds it full runs again.
- Loads never see a bound slot or a pair: loading such a word is the program's error, counted by the reference
  (`gvm_core.stale`), and a check fails on it.
- `GvmAsm::file_paths` (set by gvmbench from the tick's XFAST) puts lf, sf and fpair in paths 3 and 5 (with a lite op 1);
  without it they are the general step's.

The tick keeps the file as `uint4` arrays in local memory (XR / 4 temporary registers each for values and tags), loaded
whole at the pass's start (`xload=pre`) or a texel at its first use (`lazy`, a bit a texel), and written out with the state
at the pass's end (always: the shader spills it, no program code); the commit draws CPTS = WCN + XR (XBIND) + NPAIR points
a core. Check cases `xfile`, `xpair`, `xbind` (two cores, random slots, binds read, changed, unbound, then loaded).
