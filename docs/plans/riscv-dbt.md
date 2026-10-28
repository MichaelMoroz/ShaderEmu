# Translating code inside the tick: a study (28 October 2026)

The question: should the RISC-V tick (`experiments/rvc_opt`, D3D11 DXBC by fxc2) translate guest code while it runs?
The idea is that a pre-decoded or fused form of an instruction or a block would run faster than the raw RISC-V word.
The tick would do the translating itself, in the shader.

**The answer is no.** The most that a pre-decoded form could save is about 7 to 14% of an instruction's time, and that
assumes decoding costs nothing. The costs that translation itself adds are about as large as that. The same kind of
change has already been measured on this tick three times, and each time it was level or slower. Fusing pairs could
save at most about 2 to 3%, and the new kinds it adds to the tree cost about that much. A cheaper alternative, removing
PIE's GOT loads, may be worth 1 to 3% and needs no tick change. Even so, nothing here reaches the 5% that would justify a
prototype, so none was built and nothing was run on the card.

## What this rests on

Earlier results were read first. They already settle most of the question:

- `docs/machine/tick-costs.md`, 13 October 2026, second round:
  - A pre-decoded form was among the shapes looked for, and none of them gave a structure with fewer operations.
  - Fetch and decode for nothing would be about 25 ns of an instruction's 220 to 270 ns.
  - A trial took four of the existence tests out of decode (arithmetic and jumps). It was **1.0 to 1.4% slower**,
    though it gave the same state.
  - The loop's body run twice a turn was -2.9 to -4.6%.
  - The tests' results worked out first as bools were -0.5 to +4.6%, against +4.1 to +8.9% when the tests are written
    in the ifs.
  - The lesson drawn there: the shape of the tests is what gains, not how many operations there are.
- `experiments/rv32_min/README.md`, "What did not pay" (the compute machine, which has no fast path):
  - An entry with a tag and a check word (a hashed decoded table) was no faster than the raw word.
  - Pairs fused at decode were as fast as without:
    - which pairs: `addi`+`addi`, `sw`+`sw`, `lw`+`lw`, and `lbu`+branch;
    - they were 18 to 25% of the pairs;
    - one turn of a pair cost as much as one turn of a single instruction.
- gpu_vm, the custom ISA that this stack already has, built for the costs of a tick:
  - Measured per instruction, E4 runs arithmetic 2.40x, loads 1.44x and stores 1.31x as fast as lean RISC-V. Games
    carried by core 0 gain only 1.1 to 1.2x (`riscv-isa.md` section 7; GPU-VM's RESULTS).
  - Its probes of block-shaped ticks (`tick-costs.md`, "Blocks") found the following:
    - The fetch paid once a block gave 1.31x on straight integer code and 1.16 to 1.27x on the games' mix.
    - A block whose length and kinds are read from the code, as any translated block must be, was **slower** than
      today's shape: 94.8 ns against 77.6 ns for integers, and 176 ns against 112 ns for the mix.
    - That search was closed below 1.4x.
- `riscv-backport-plan.md` already says "predecoding would gain little: RISC-V's decode is cheap already". It also
  lists "fused instructions, bundles, a new encoding" among the ideas not for RISC-V.

## 1. Where a fast-loop instruction's time goes today

### In bytecode

This is the fast loop of the current tick: `build/after`'s blob `c6e1bc56faeba478.cso`, disassembled by
`d3dcompiler_47`.

- The loop is 2,253 DXBC instructions; the whole shader is 13,396, with 384 temps.
- The paths below were walked by hand on its if-tree.
- A register-arithmetic instruction whose word is in the texel already turned (`fast_same`) runs 91 DXBC instructions:

| part of the step | DXBC instructions (count) | what they are |
|---|---|---|
| fetch, same texel | 8 | the `fast_same` test; pc, `fast_same` and the window's turn at the end |
| fetch, a new texel (25 to 34% of instructions) | +28 | the texel number, the read-ahead texel taken or read, the next one read, the turn by four selects |
| fields | 10 | opcode, funct3, rd, rs1 and rs2 indices, the word kept |
| the two register reads | 2 | `x2[...]` (local array) |
| npc and the step's flags | 5 | |
| the kinds' tests and the tree | 16 | 13 compares and ors, 3 `if`s |
| existence (funct7 table) | 10 | |
| execute (every result, one chosen; the immediate) | 32 | |
| result write and `ok` | 5 | the `if_z ok` test and the branch-free write |
| the loop's end | 3 | the count and the budget |
| **all** | **91** | 119 with a new texel |

- **A load** adds about 60 to 75 DXBC instructions after decode:
  - the address and its tests (17);
  - the data texel asked for (7, behind its test);
  - the write cache's filters (`bl`, `L1_KNOWN`, `L1_HAS`: 4 to 17);
  - the word taken out of the texel (26).
- **A store** adds about 80 to 100. A new texel in the write cache adds a bucket search on top of that.

### In time

These are the costs fitted per kind (`tick-costs.md`, "What a pass is made of"):

- an arithmetic-like instruction 115 ns;
- a load 450 ns;
- a store 370 ns;
- the general path 2,700 ns (0.4% of instructions).

They are weighed by each part's mix from its `RVC_PROF` profile (`tools/rv_compare/dbt_model.py`). The float rows use
150 ns, the light float path.

| part | arithmetic, branches, jumps (% of instructions) | loads (%) | stores (%) | floats (%) | mean (ns an instruction) | loads and stores (% of the time) |
|---|---|---|---|---|---|---|
| Doom timedemo, core 0 | 61 | 26.9 | 11.7 | 0 | 246 | 67 |
| Quake timedemo, core 0 | 52 | 19.7 | 12.3 | 15.9 | 229 | 59 |
| Tiberian Dawn, core 0 | 65 | 25.2 | 9.8 | 0 | 235 | 64 |
| gzip (stands in for the shell loop) | 69 | 21.6 | 9.1 | 0 | 221 | 59 |
| Quake III, core 0 | 59 | 20.5 | 12.3 | 7.8 | 228 | 60 |
| Half-Life, core 0 | 52 | 21.0 | 15.4 | 11.3 | 239 | 63 |

Most of the time goes to loads and stores: 57 to 67% of it. A load's or store's cost is the texture's latency (35 ns
when the texture cache has the texel, about 90 ns when it does not) and the write cache's search, not its decode.
Fetch and decode together are about 25 ns of the mean (`tick-costs.md`), and the fetch part is mostly hidden by the
read-ahead.

### What a pre-decoded form could remove

Suppose an entry carried an op index, the register numbers and a sign-extended immediate. Then the following would go:

- most of the fields: 6 of 10;
- the kinds' compares: 13, leaving 1 to 3 to read the index;
- the existence table: 10;
- the immediate's shift: 1.

That is about 29 DXBC instructions of 91. Here is what that is worth in time:

- An operation costs 0.55 ns when nothing waits for it and 1.1 to 1.3 ns on a chain (`tick-costs.md`).
- So the 29 instructions are worth 16 to 35 ns of a 221 to 246 ns mean: **7 to 14% at the very most**.
- The only measured piece of it points the other way: four of the existence tests taken out was 1.0 to 1.4% slower.

The register reads, the execution, the load and store paths and the loop do not change.

### What fusion could remove

These are pairs next to each other in which the second instruction reads the first one's result
(`tools/rv_compare/dbt_model.py`, greedy along the code). The figures are upper bounds, as % of the part's instructions
that a fused kind would absorb:

| part | `lui`+use (%) | `auipc`+use (%) | `li`+branch (%) | `addi`+branch (%) | compare+branch (%) | load+branch (%) | other arithmetic+branch (%) | `add`+load or store (%) | load+arithmetic (%) | arithmetic+arithmetic (%) | the first five (%) | all (%) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| Doom, core 0 | 0.38 | 3.60 | 0.85 | 0.47 | 0.33 | 6.23 | 0.91 | 1.17 | 1.05 | 5.22 | 5.6 | 20.2 |
| Quake timedemo, core 0 | 0.30 | 1.70 | 1.18 | 0.63 | 0.63 | 1.84 | 0.69 | 1.33 | 0.76 | 3.83 | 4.4 | 12.9 |
| Tiberian Dawn, core 0 | 0.50 | 3.09 | 1.51 | 0.52 | 0.87 | 3.58 | 0.78 | 0.92 | 3.00 | 5.19 | 6.5 | 20.0 |
| gzip | 0.77 | 0.42 | 0.72 | 3.24 | 0.24 | 7.24 | 0.58 | 4.11 | 0.89 | 10.12 | 5.4 | 28.3 |
| Quake III, core 0 | 0.99 | 4.70 | 0.86 | 0.19 | 0.47 | 1.71 | 0.37 | 1.15 | 0.78 | 3.20 | 7.2 | 14.4 |
| Quake III, workers | 0.61 | 3.45 | 0.76 | 0.24 | 0.41 | 2.44 | 0.62 | 0.91 | 0.65 | 3.21 | 5.5 | 13.3 |

"The first five" are `lui`, `auipc`, `li`, `addi` and compare, each with its use.

- **What an absorbed instruction saves.** It saves a loop turn, its fetch and its decode: at most about 95 ns of an
  arithmetic one's 115, leaving its own execution. gpu_vm's floor gives about 40 ns: two adds in a step cost 63.9 ns,
  one 52.7 ns.
- **The ceiling.**
  - The first five pairs, at 95 ns each, are at most 1.8 to 3.0% of the mean.
  - Every pair, if each were free, would be 5 to 7%. But a load+use pair does not hide the load's latency, which is
    most of its cost.
- **What each kind costs.** Each fused kind is a new place in `fast_exec_l1`'s tree. A place in that row costs every
  kind after it 16 ns, and changes to the tree have cost 1 to 3% of every instruction.
- **What was measured.** The pairs measured on `rv32_min` came out level.
- **Where `auipc`+use comes from.** It is mostly PIE's addresses and GOT loads, which belong to the linker, not to a
  tick or an ISA (below).

## 2. Translation inside the shader: the constraints

- **Where translated code would live.**
  - A pixel writes only its own eight targets, so a core's translations would have to be either in its state texels or
    in RAM.
  - A core's state is about 500 texels (512 for an 8 x 8 block).
  - The code a run touches is larger than that (`dbt_model.py`, texels of raw code covering 90 / 99% of the run):

    | part | texels for 90% of the run (count) | texels for 99% (count) |
    |---|---|---|
    | Doom | 2,738 | 7,763 |
    | Quake timedemo | 3,292 | 10,910 |
    | Tiberian Dawn | 5,106 | 15,849 |
    | desktop | 6,396 | 21,819 |
    | Quake III, core 0 | 2,055 | 4,719 |
    | gzip | 707 | 1,790 |

  - A pre-decoded entry needs at least 64 bits: an op index, three registers and a 32-bit immediate. That is two to a
    texel, so a translation takes twice the texels above.
  - So translations must go to a region of RAM, and the only way into RAM is the commit, through the write cache of 336
    texels a pass.
  - Translating Doom's 99% would be about 15,500 texels, which is 46 passes' whole write cache. That is paid again
    after every `fence.i` flush (below).
- **Who translates, and when.**
  - **The core itself, on a miss.** It must interpret the block anyway in that pass, since what it stores is seen only
    from the next pass on. So a miss costs the plain step, plus a decode written out, plus a store: about 0.5 to 0.8 µs
    an instruction.
    - At no more than about 20 ns gained an execution, a block pays back after 25 to 40 runs.
    - Doom's 113,000 distinct instructions would cost about 70 ms of core 0 a run before any gain.
  - **A worker or a lane job.** Workers store only through their own write caches and pass boundaries, and someone
    still has to find the misses and ask for the work.
  - **Ahead of time.** This is option B below.
- **Lookup on every block entry.**
  - A taken jump or branch is 7 to 12% of instructions (runs of 8 to 14 between them). A new texel is entered at 25 to
    34% of instructions.
  - Today such an entry reads the target texel: 35 to 90 ns, overlapped by the read-ahead on straight code.
  - A translation needs that same read, plus a tag compare and branch (16 to 19 ns, a test) at each entry. A hashed
    table instead of a fixed shadow address would need a second dependent read (35 to 90 ns).
  - With twice the texels per instruction, the read-ahead covers two instructions instead of four. So the new-texel
    path (+28 DXBC and an exposed read) runs twice as often in straight code.
  - The cost of a jump texel for nothing is 3 to 4% (`tick-costs.md`). Doubling texel entries is of that order.
  - Together these costs are about 3 to 8% of the mean, which is the size of the realistic decode gain.
- **Both decoders in the loop.** A miss falls back to the raw step, so the fast loop holds both the raw tree and the
  translated one. A kind added to the tree has cost 1 to 3% of every instruction.
- **Invalidation.**
  - The tick never checks a store against fetched code. A fetch reads RAM as last committed, and RISC-V requires
    `fence.i` before running stored code.
  - A generation word in each tag, raised at every `fence.i`, is therefore enough. The poke flag (`poke_hook.py`), a
    flat program's load and an exec'd mapping all come through `fence.i`.
  - But every `fence.i` throws away everything, and the NES's 6502 recompiler (`rc6502.h`) makes code all the time.
    Checking stores against a code bitmap instead would be a test on every store: 16 to 19 ns on 9 to 16% of
    instructions.
- **The write cache and the pass.**
  - Translation stores compete for the 336 texels that already end core 0's passes in Doom (3% of its passes) and Quake
    (18%).
  - A pass that ends early in VR loses the rest of its round. So in VR, translation is a cost until the working set is
    warm, and again after each flush.
  - The 8,192-instruction pass is unaffected, except that a miss's extra work is paid inside it.
- **VRChat.** Nothing here needs a UAV or a texture array. It is all reachable through the commit, so this is not what
  rules it out. The numbers above are.

## 3. The options compared

The estimates below are the tick's time per instruction in the harness (fps under a fixed amount of work) and core 0's
instructions per round in VR, against today's tree.

### A. Translation in the shader

- **Upper bound.** At most +7 to 14% from decode, before any of the costs in section 2.
- **Realistic.**
  - The gain is the bound less the existence test's measured -1%.
  - The costs are twice the texel entries, the entry's tag test, the raw tree kept beside the translated one, and the
    warm-up texels.
  - Net about **-6 to +3%** in the harness. VR is worse: translation stores eat the write cache.
- **Cost.** Large: a new layout in RAM, generations, the loop rebuilt, Unity's copy. The risk is high, because shapes of
  this loop move by 1 to 9% unpredictably.

### B. Pre-decoding at load time (the kernel or the ROM tool writes a decoded image beside `.text`)

- **What it removes.** In-pass translation, misses and warm-up.
- **What it keeps.** The wide entries (twice the texel entries) and a way from pc to the decoded word. A per-process
  offset costs one add and no tag. A kernel or ROM tool also has to own it for every executable mapping and every
  `fence.i`.
- **Estimate.** About **-4 to +5%**, centred near 0.
- **Cost.** Medium to large: the kernel, the ROM tool, `rvc_cpu`, the tick, and memory for the image (twice `.text`).

### C. A JIT in the guest, on worker cores

- **Estimate.** As B, with run-time costs added: jobs, a pass of latency, and coherence with core 0's stores. **About 0
  or worse.**
- **Cost.** Large.

### D. Programs built for a richer encoding

The whole stack is ours, so the programs can be compiled for a different encoding.

- **D1. Zba, Zbb and Zbs, then `fmadd` fast** (`riscv-isa.md`).
  - The harness: +1.8 to +3.7%, and Quake about +2.5%, nxray +5% with `fmadd`.
  - VR: +2 to +3.5% of core 0's work a round.
  - Cost: small. The tick's `ZB` exists; the toolchain is rebuilt. The card's check is still owed there.
- **D2. Fused custom opcodes** (`add`+load or store indexed; compare or `li` or `addi` and branch).
  - The bound: 2.7 to 3.8% of instructions absorbed (gzip 8.3%), the sum of the `add`+load or store, `li`+branch,
    `addi`+branch and compare+branch columns. At 95 ns each that is +1.1 to +1.6% (gzip +3.4%).
  - Against it: 1 to 3% for each new kind in the tree.
  - Net about 0. Cost: medium to large, with an LLVM or GCC pass and an assembler.
- **D3. An encoding with the kind in fixed bits and no existence table.**
  - This removes most of the 29-instruction decode bound.
  - It is the first step towards gpu_vm, which this stack already has. gpu_vm is the measured end of this road: games
    carried by core 0 at 1.1 to 1.2x.
  - It is not a change to the RISC-V machine.

### E. GOT loads out of PIE code (the linker; not in the brief, found here)

`tools/rv_compare/dbt_got.py` counts each `auipc` with what reads its result next:

| part | `auipc` (% of instructions) | then a GOT `lw` (%) | then `addi` (%) | then `jalr` (%) |
|---|---|---|---|---|
| Doom, core 0 | 3.60 | 1.06 | 2.21 | 0.04 |
| Quake timedemo, core 0 | 2.42 | 0.53 | 1.08 | 0.04 |
| Quake e1m1, core 0 | 6.13 | 1.70 | 2.07 | 0.05 |
| Quake III, core 0 | 5.27 | 1.59 | 2.67 | 0.29 |
| Quake III, workers | 4.36 | 1.79 | 1.37 | 0.12 |
| Half-Life, core 0 | 2.60 | 0.93 | 0.80 | 0.35 |
| Tiberian Dawn, core 0 | 3.09 | 0.27 | 2.25 | 0.33 |

- **Why it could pay.** A GOT `lw` is a load: 450 ns, and a data texel read.
- **What would remove them.**
  - Relaxing GOT loads of local, non-preemptible symbols to `auipc`+`addi`.
  - Or linking non-PIE and letting bFLT's relocations fix the absolute addresses.
- **Estimate.** By the cost model that removes 0.3 to 1.8% of instructions as loads, about **+1 to +3%** (Doom about
  2%, Quake and Quake III about 3%), with no tick change at all. VR gains about the same, with fewer data texels read.
- **Before trusting it.**
  - `riscv-backport-plan.md` keeps every program PIE through rv32-cc on purpose (a GOT load for an extern global).
  - Whether GNU ld relaxes these, or a non-PIE flat link works with bFLT's relocations, is for the toolchain's area to
    check.

## 4. The ranking

| rank | option | harness, flat out (%, estimated) | VR, core 0's work a round (%, estimated) | cost to build | verdict |
|---|---|---|---|---|---|
| 1 | D1: Zb, then `fmadd` fast | +1.8 to +3.7 (Quake +2.5, nxray +5) | +2 to +3.5 | small (exists; the card's check owed) | take, as `riscv-isa.md` says |
| 2 | E: GOT loads out of PIE code | +1 to +3 (model) | +1 to +3 | small to medium (toolchain, all programs rebuilt) | check whether the link allows it, then measure on `rvc_cpu` |
| 3 | D2: fused custom opcodes | about 0 (bound +2, each kind -1 to -3) | about 0 | medium to large | no |
| 4 | B: pre-decoded at load time | -4 to +5, about 0 | about 0 | medium to large | no |
| 5 | C: a JIT on the workers | about 0 or worse | worse (latency, stores) | large | no |
| 6 | A: translation in the shader | -6 to +3 | worse (the write cache) | large, high risk | no |
| - | D3: a new encoding | that is gpu_vm: 1.1 to 1.2x on games' core 0 | - | a new ISA (exists) | not for the RISC-V machine |

None of these is a 2x lever. Translating code in the tick does not pass the bar of more than 5% on the shell loop and
Doom's timedemo together, so no prototype was built and nothing was run on the card.

## What would change this answer

- **A card measurement of a decode bound.** For example, a variant whose arithmetic decode is replaced by a
  table read keyed by the word's low bits, as an upper bound. The existence-test trial of 13 October already says this
  is level to negative. Only if that variant gained more than about 8% on the shell loop and Doom would option B be
  worth building.
- **A future tick whose loads and stores are much cheaper.** Today they are 57 to 67% of the time. If they fell, fetch
  and decode would become a larger share, and this study would need doing again.

## Files

- `tools/rv_compare/dbt_model.py`: each part's mix, run lengths, the fusion bounds and the code each run touches, from
  `RVC_PROF`'s `pcs.txt`.
  - It was run on `ShaderEmu-bp-isa/build/isa/runs/*_base.prof` and `ShaderEmu-bp-prof/build/prof_parts/*.prof`.
- `tools/rv_compare/dbt_got.py`: each `auipc` with what reads its result next.
- The bytecode walk was done on `build/after/build/shadercache/c6e1bc56faeba478.cso` of the `riscv-backport`
  comparison, disassembled with `tools/cso_loops.py`'s `listing`.
