# gpu_vm's tick built up in steps

Each step is a shader of its own (`xN.hlsl`), run by `bin\gvmbench.exe --shader gpu_vm\tick\steps\xN.hlsl` on the
cases it can make and checked against the reference machine (`gpu_vm/ref/gvm_ref.h`: registers, pc, the count of
ops, every word of RAM). Built with `gpu_vm\tick\build.bat` (MSVC; the compiler beside the exe is fxc2,
`docs/machine/fxc2.md`), timed on the RTX 5070 Laptop through `tools\gpu_queue.ps1`, 1,000 passes of 8,192
bundles, one core. The timed loops are `experiments/isa_proto/lean/isaloops.c`'s written in gpu_vm, scored in that
file's RISC-V instructions a second (the lean tick's measure).

The start is `experiments/isa_proto/steps/s21.hlsl`, the last verified step of the prototype; the target is two
to three times the lean tick on the same loops. gpu_vm's instruction set is not fixed: an encoding, a field, a path or
a new op (a fused one too) changes wherever the tick is faster for it, with `gpu_vm/isa.h`, the reference and the
assembler.

## The two machines it is held against (this card, 22 October 2026)

Millions of RISC-V instructions a second. The lean tick: `isaloops 16` in a private no-MMU image
(`build/images/lean_isaloops` of the main checkout, the prebuilt root and the program built by `~/nm/work/bin/rv32-cc
-O2` and `linux/nommu/elf2bflt.py`), `rvc_harness --d3d11 --rvc experiments\rvc_lean`, the second of two rounds.
s21: `isabench --wordwc` with `cbuffer Pass` renamed (FXC and fxc2 both take `Pass` for a keyword now).

| | arith | loads | stores | adds | mix | tight | stride |
|---|---|---|---|---|---|---|---|
| lean tick | 7.83 | 5.49 | 4.63 | 7.75 | 6.85 | 6.88 | 6.04 |
| s21 | 20.6 | 12.1 | 9.8 | 19.0 | 16.8 | 19.1 | 16.7 |
| two times the lean tick | 15.7 | 11.0 | 9.3 | 15.5 | 13.7 | 13.8 | 12.1 |

## Steps

| Step | What it adds | ns a bundle | arith | adds | tight |
|---|---|---|---|---|---|
| x1 | gpu_vm's bundle: three slots of a sum (add, addi), the control word's branches and `j`; reads first, results to later ops by selects, writes last | 140 | | 23.1 | 14.4 |
| x2 | the integer unit's 24 functions in every slot (`isa.h`'s numbering: a tree of selects) | 255 | 11.1 | 12.6 | 7.8 |
| x3 | x2 with a body for each count of ops (1, 2, 3), chosen once a bundle | 187 to 510 | 5.6 | 10.1 | 10.7 |
| x4 | slot 0 the whole unit, slots 1 and 2 sums, logic and the three shifts (f 0-10); the assembler opens a bundle for an op that would land in a slot without its function | 220 | 12.9 | 14.5 | 9.1 |
| x5 | x4 told by the control word: L bits mark the long immediates, the path says which body runs the bundle (no op looked at to decide) | 200 | 14.1 | 16.1 | 10.1 |
| x6 | two op slots (word 2 the long immediate of the op the control word names), the branch on the registers as the bundle found them, r0's writes into r64 | 125 | 16.9 | 15.9 | 16.0 |

Against the lean tick, x5 is 1.80 (arith), 2.07 (adds) and 1.46 (tight); x6 2.16, 2.06 and 2.32.

From x6 on gpu_vm has two op slots, and its branches compare the registers as the bundle found them (decided 22
October 2026 on these numbers: `gpu_vm/isa.h`).

| Step | Memory | ns: an integer bundle, a load's, two loads' | arith | loads | stores | mix | stride | tight |
|---|---|---|---|---|---|---|---|---|
| x7 | path 2: op 0 a load or store (b, h, w, aligned, in RAM), op 1 a cheap integer op; a write cache of word pairs, two ways of 64; the commit (four draws) | 136, 216, | 15.5 | 5.30 | 4.33 | 12.8 | 11.9 | 14.8 |
| x8 | path 3: both ops loads or stores; op 1's store with no room undoes op 0 | 169, , 313 | 12.5 | 6.39 | 4.87 | 10.7 | 10.0 | 11.8 |
| x9 | one `mem()` for both ops, a loop of one or two trips (x8's three copies made the loop 779 DXBC instructions; this 207) | 152, , 394 | 14.0 | 5.08 | 3.90 | 10.2 | 9.8 | 13.2 |
| x10 | no `break` in the loop (a stop zeroes the budget) | 158, , 400 | 13.5 | 5.01 | 4.14 | 10.1 | 9.7 | 12.9 |
| x11 | loads as s21 makes them: both ops' addresses, texels and cache entries read at once, words chosen by selects (stores halt) | 165, , 222 | 12.6 | 8.99 | | | 10.6 | 12.4 |

The lean tick's loads 5.49, stores 4.63, mix 6.85, stride 6.04: x11 is 1.64 on loads and 1.76 on stride, and its
integer bundles 1.6. s21's trip of two loads is 166 ns against its integer trip's 103; x11's 222 against 165.

## x12: every fast path, and variants in batches

x12 has integers, loads and stores (s21's form, a store with no room undoing op 0), floats (the float unit
renumbered for a tree of selects, the special functions behind a branch) and memory with a float sum: paths 1-6
(`gpu_vm/isa.h`, the path now 3 bits; branches 9 bits, j and call 18). Every check case is the reference's: integers,
conditions, memory, a full write cache, floats exact, floats in every path, and division and the special
functions within 2e-5 (`sfu`).

The lean tick was measured again in the session of x12 with `isaloops`' two float loops (floats: sums,
products, min, max; fmix: float loads, products and a store): arith 8.26, loads 5.70, stores 4.91, adds 8.18,
tight 7.28, mix 7.22, stride 6.37, floats 6.11, fmix 5.31 (5% above the first session on every loop: the card's
drift). `gvmbench --variants FILE` checks each variant on the check cases, then times the loops in rounds,
forward and backward, and prints the median and the geometric mean against these (`gpu_vm/tick/variants/`).

| Batch | What it tried | Best, x lean |
|---|---|---|
| x12a (20) | prefetch, write cache 64/128/256, register array 65/72/128, op 1's memory read late, integers tested first, a switch on the path, the assembler's path trusted (`OK_MIN`), r0 cleared after a bundle, stores without a branch, the plain hash, and pairs of them | `OK_MIN` + `INT_FIRST` 1.41 (base 1.30) |
| x12b (10) | loads apart from stores (x11's form for a bundle with no store) | none better (1.37 to 1.42) |
| x12c (7) | probes: parts compiled out | loads 6.05 to 7.86 with no store code at all, though `LOADS_APART` runs none of it |
| x12d (10) | one-trip loops round each body, so the card's compiler cannot run it predicated | none better (1.17 to 1.41) |

## Nsight Graphics (22 October 2026)

`bin\gvmtrace12.exe` runs a step on D3D12, compiled by DXC with its debug information in it (`-Zi -Qembed_debug`)
or as fxc2's DXBC (`--dxbc`): arith 14.8 and 14.6 M a second, as the D3D11 bench. `pwsh gpu_vm\tick\trace.ps1 -Case
loads` takes a GPU Trace with the real-time shader profiler (`logs\nsight-gvm\...`: the counters exported, the
`.ngfx-gputrace` for Nsight's UI, where the shader's own code is against the source lines). x12 on loads, the
tick's warps:

| Warp state | Share |
|---|---|
| waiting on a fixed-latency dependency (an ALU chain) | 40.5% |
| issuing | 35.2% |
| waiting on texture or local memory | 9.5% |
| no instruction (fetch, after a branch) | 8.5% |
| resolving a branch | 5.4% |

Memory's latency is a tenth even of a loop of loads: a bundle's time is its instructions and the chains between
them. A warp instruction runs 12 of 32 lanes (a core's 11 pixels, the helpers leaving at once).

## What was measured on the way

- **A core's pixel rows.** A core is a row one pixel high: the quad's second row runs as helpers, reads empty
  state and trapped at pc 0 for its whole budget beside the real core. Pixels of no core leave at once (all steps).
- **The branch's fourteen conditions** as a chain of selects cost 27 ns of x1's 150; as a kind (equal, less,
  unsigned less, a bit) and an inverse, with the signed compare by xor of the sign bit, 10 (x1).
- **Empty slots cost what full ones do** (x1: one op 140 ns, three 140; the first slot alone 91). Behind
  branches they cost less in a bundle of one op (126) and more in a full one (168); reads first with the work
  behind branches 186. Unconditional is kept: packed code is what a compiler makes.
- **The integer unit.** Every result made and chosen by a chain of selects: 390 ns (x2 as first written). A
  `switch` on the function: 253 to 355. One branch between sums, logic and shifts and the rest: 297; then each
  part: the branch itself with a sum on its side 212, logic 249, shifts 267, rotates 297. Renumbered so that
  every result is made in parallel and three levels of selects on f's bits choose (`isa.h`): 255. The same
  without compares, min, max, mul and rotates: 178 (13 ns a slot over a sum; the rest 26 more).
- **A body for each count of ops** (x3): a bundle of one op 187 ns, but three bodies of the whole unit and the
  branch among them made full bundles 317 and arith 510.
- **Deciding from the ops.** Looking at each word for a long immediate and checking each op was one the step
  makes: about 100 of the loop's 400 DXBC instructions, 33 ns (x2 `NO_DECODE`). The control word's L and path
  bits remove it (x5: 220 to 200).
- **Where x5's 200 ns go** (probes in `x5.hlsl`): the same bundle with sums in every slot 140; two op slots in
  place of three 183; the control word comparing the registers as the bundle found them, no forwarding (`BR_EARLY`)
  183; r0's writes into a register 64 rather than r0 cleared every bundle 196; slot 0 the cheap unit with the rest
  behind a branch: 185 for a cheap op, 250 for arith (its `sltu` takes the branch); two slots, the early branch
  and the sink together 148. A branch on the last op's result (`BR_RES`) is slower, 194 to 260: the op lies on
  the chain to the next fetch (as s14 found).
- **The instructions are the cost.** x5's loop is 303 DXBC instructions, s21's 215 of which about 145 run on an
  arithmetic trip: both about 0.7 ns an instruction. A slot's fixed work (fields, two reads, the selects that
  pass results on, a write) is about 25 of them, the whole unit about 45, the cheap one 20.
- Declared register arrays of 64, 65 and 128 are alike; a write to the empty slots skipped by branches (x1
  `W_COND`) alike.
- **A load.** The write cache's lookup and the byte and half extension are 47 and 14 ns of x9's 394 for two loads
  (`NO_WC`, `NO_EXT`); the fetch hoisted out of its branch is not it (a loop of one trip round it, `FENCE_LOOP`: no
  faster). What made loads cheaper was s21's form (x11): everything both loads need read at once, chosen by selects,
  one wait for memory a bundle.
- **A path's branch costs every bundle.** x6 with its integer unit on one side of a branch whose other side is
  trivial (`BR_PROBE`): 126 to 148 ns. Every memory step's integer bundles pay it, and more when the other side is
  large (x8 169, x9 152, x11 165), whether that side's code runs or not. Declared sizes do not move it (x11: the
  write cache 64 or 128, the registers 65 or 128, within 4%).
- **A result written a bundle late**, the next bundle's reads taking it by selects (x6 `LATE`): no faster (15.5
  against 15.8): a read does not wait for the store before it.
- **The card drifts**: x6 measured 16.9 and 15.8 on arith an hour apart. Compare in one session, A then B.

## x13: two loops

x13 is x12 in two loops: the fast loop makes paths 1-3 (integers, loads and stores) and has no float code compiled
into it; a bundle of another path leaves it for one trip of the full step (x12's), and the fast loop takes over
again. The write cache's entries are cleared when a pass starts (128 stores a pass), so a tag of 0 is an empty
slot and x12's bit a slot (`occv`) is gone. `gpu_vm/tick/dxbc.py` prints a step's DXBC as fxc2 compiles it, with no card.

| Batch x13a | arith | loads | stores | adds | tight | mix | stride | floats | fmix | x lean |
|---|---|---|---|---|---|---|---|---|---|---|
| x12 best | 13.62 | 6.03 | 5.17 | 12.52 | 12.12 | 10.81 | 9.92 | 7.87 | 6.91 | 1.38 |
| x13 | 14.00 | 6.47 | 5.72 | 13.26 | 13.34 | 12.20 | 10.66 | 6.92 | 6.92 | 1.45 |
| x13, one loop | 13.42 | 6.15 | 5.42 | 12.63 | 12.64 | 11.21 | 9.87 | 7.84 | 7.04 | 1.40 |

The second loop is worth 4% on the integer and memory loops, and costs the float loops 12% (a trip of the outer
loop a float bundle). Most of x12's distance from x6 (arith 16.9) and s21 (loads 12.1) is not the float code.

### Round 1 of variants by subagents (x13i, x13m, x13s, x13f; 22 October 2026)

Four subagents each made a copy of x13 with switches (checked on WARP), and the lead ran each batch on the card.
"x lean" over the seven integer and memory loops (arith, loads, stores, adds, tight, mix, stride), x13 1.53:

| Batch | Best | x lean | What it showed |
|---|---|---|---|
| x13i (integers, a bundle's fixed cost) | `i_all`: the fast loop's own step (pc in texels, no halt, a store with no room undone in its branch), a cheaper unit (rsub as a negated difference, shifts by y itself, one compare), cc - 1's bits as the kind, an empty slot's rd 0, whose long immediate by one test | 1.65 | 41 DXBC fewer an integer trip: arith 14.1 to 16.1; an integer-only inner loop (`i_il`) arith 16.25 but loads 6.0 |
| x13m (memory) | `mflat2w1`: load values flat, a store's work behind one branch, a direct-mapped cache with a victim slot | 1.48 (all ten loops) | loads 6.5 to 7.2; deciding the branch first (`BR_FIRST`) 15% slower |
| x13s (storage, structure) | `s_combil`: trims and an integer loop | 1.65 | the register file as a local array, the write cache as uint4 pairs: no change |
| x13f (fused ops) | `addbnz` (cc 15: ra += imm6, taken when not 0) | | free on code without it; loads +4%, adds +15%, stride +29% where loops end in it. The control word's add (cc 0 sub 7) costs every bundle 4% (mix +16%): not taken further |

Probes on x13 (they fail the checks): op 1 reading the registers as the bundle found them, +3% on integers; no
texel read for data, loads 6.5 to 7.5; no write cache read either, 7.9. A load bundle's cost was its own
instructions and branches (about 270 DXBC against 130 for an integer bundle), not memory's latency.

## x14: the encoding of 22 October 2026

`gpu_vm/isa.h` changed for it (steps x1-x13 ran the encoding before and no longer pass): loads and stores have paths
of their own (2 a load and lite, 3 two loads, 4 a store and lite, 5 two memory ops with a store), a memory pair's
op 1 reads nothing op 0 writes (the assembler opens a bundle), the long immediate's bits are one an op, an empty
slot is a word of 0, a bundle with jr, callr, call or eret is path 0, floats are path 6 (op 1 lite or float lite)
and 7 (memory and float lite), and cc 15 is addbnz. The fast step is i_all's; loads are s21's form, flat; stores
know both slots before writing either (no undo; a write cache entry past the end takes a store not made).

| | arith | loads | stores | adds | tight | mix | stride | x lean (7) |
|---|---|---|---|---|---|---|---|---|
| x14, loops as before | 15.47 | 9.37 | 7.85 | 14.55 | 14.55 | 13.17 | 11.60 | 1.79 |
| x14, loops ending in addbnz (`--fused 1`) | 15.42 | 10.26 | 8.39 | 16.58 | 14.52 | 13.15 | 15.21 | 1.93 |
| two times the lean tick | 16.5 | 11.4 | 9.8 | 16.4 | 14.6 | 14.4 | 12.7 | 2 |

### Round 2 (x14i: the integer bundle and a bundle's fixed cost)

| Variant | What | arith | loads | stores | adds | tight | mix | stride | x lean (7) |
|---|---|---|---|---|---|---|---|---|---|
| x14 | | 15.41 | 10.26 | 8.38 | 16.57 | 14.50 | 13.13 | 14.88 | 1.93 |
| i_cc | `taken` as one unsigned compare chosen by bit tables on 1 << cc (fxc had turned x14's bools back into 1 and 0) | 17.56 | 11.38 | 8.83 | 18.91 | 16.52 | 14.41 | 16.14 | 2.14 |
| i_one | the branch first, one fetch of the next texel | 15.28 | 10.27 | 8.39 | 16.44 | 14.40 | 13.12 | 14.87 | 1.92 |
| i_dec | I_CC, I_ONE, I_TRIM (rsub by one test, the target's shift chosen) | 17.72 | 11.70 | 9.13 | 19.07 | 16.68 | 14.73 | 16.40 | 2.17 |
| i_l1 | integers and memory in sibling loops | 14.01 | 10.58 | 8.74 | 15.07 | 13.25 | 11.56 | 13.36 | 1.81 |
| i_l1all | that and i_dec | 19.48 | 11.38 | 9.39 | 20.94 | 18.33 | 13.60 | 14.86 | 2.21 |
| i_l2all | an inner do-while of integer bundles, and i_dec | 20.39 | 10.70 | 8.53 | 21.96 | 19.21 | 13.76 | 14.79 | 2.20 |

i_dec and i_l2all are the reference's on the card too (every case, `--fused 1`). i_one alone is no faster but helps
beside I_CC: 8 DXBC removed from the branch's test were worth 11%. i_dec is two times the lean tick on every loop
but stores (9.13 against 9.8); i_l2all is faster on integers and slower on mix and stores.

## x15: the encoding cut for the fast loop's decode (22 October 2026)

`gpu_vm/isa.h` changed again (x14 and the steps before it read the old encoding and no longer compile or pass). DXBC an
executed trip of the fast loop, counted from fxc2's listing (`dxbc.py --list`):

| Trip | x14 | x15 |
|---|---|---|
| integer (path 3: op 0 mid, op 1 lite) | 199 | 150 |
| loads (paths 4, 5: one load and lite, or two loads) | 229 | 188 |
| stores (paths 6, 7: a store and lite, or a store and a load or store) | 280 | 226 |

What changed, and what each saved (in an integer trip unless said):

- **The control word's path at the bottom** (`ctl & 7`), the fast loop's paths 3-7 (`path >= 3`: two DXBC where
  x14's `- 1 < 5` took four); in 4-7 bit 0 is "two memory ops", bit 1 "a store". 0 general, 1 ALU (the whole integer
  unit or a float op, op 1 lite or float lite: the full step's), 2 memory and float lite. A control word of 0 is
  still illegal (path 0, cc 0 sub 0).
- **cc as fields** (bits 7-10): the inverse, less, signed (with less) or equal (without), against imm6. Neither less
  nor equal: cc 0 is special (sub, the general step's), cc 1 addbnz (taken when the sum is not 0: its "sum is 0"
  inverted), with imm6 bbs and bbc. The test has no `cc - 1`, no special or addbnz compare: 22 DXBC where x14 took 37.
  bltui and bgeui compare with imm6 sign-extended.
- **One offset field**: a near j is beq r0, r0 (its path the ops'), so the fast loop has no j and no select between
  two offsets. A j past +-4 KB is cc 0's far j (path 0); the assembler chooses in `finish()`. The control word's add
  (cc 0 sub 7) is path 0 too (x14's fast loop did not make it).
- **Fields placed for one op each**: in the op word ra at the bottom and rb at the top (imm12 over rc and rb, as
  before), rd and opc between; an indexed op's shift is `w >> 20`'s low 5 bits. In the control word cb (and imm6) at
  the top, the offset at 17-25, n at 3-4 summed as `ctl & 0x18` and shifted once a fast run (-6 together).
- **No rsub**: the I form's sub is ra - imm; sub is x + ~y + 1 (-5 over both units).
- **A mid unit for op 0**: integer functions renumbered so that 0-19 (sums, logic, slt, sltu, min, max, sge, sgeu,
  the shifts, mul) are one tree of selects and the fast loop's op 0 is that; ror, rol, seq and sne (20-23) are path 1,
  the full step's. sgt and sgtu are gone (sge and sgeu; sgt is slt with the operands swapped). Lite is 0-7 and 16-18.
  -12 (`x15full`, the whole unit in the fast loop, is 162).
- **Widths** whose low two bits times 8 are the extension's shift (w 0, d 1, h 2, b 3, q 4, hu 6, bu 7): two DXBC
  where x14's table shift took four, a memory op.
- **Path 7's op 0 is a store** (a load before a store opens a bundle): the store path makes no op 0 result and no
  select on op 0's kind (-16 a store trip).

Checked on WARP (`--fused 1` with addbnz, and without; seeds 1, 2, 3, 5, 7, 11, 99, 12345): memw, spillw, ints, conds,
float, fpaths, sfu, the seven loops and addbnz the same as the reference. int, mem, spill, branch, trap, ops, cores and
fused have ops of path 0 (the general step is a halt here, as in x14).

On the card (`variants/x15.txt`, `--fused 1`, every case the reference's there too):

| | arith | loads | stores | adds | tight | mix | stride | x lean (7) |
|---|---|---|---|---|---|---|---|---|
| x15 | 20.49 | 12.25 | 9.62 | 22.06 | 19.30 | 16.14 | 17.68 | 2.40 |
| x15full (the whole unit as op 0) | 20.04 | 12.25 | 9.61 | 21.51 | 18.85 | 15.96 | 17.43 | 2.37 |
| x15wc64 (64 entries) | 20.51 | 12.35 | 9.67 | 22.06 | 19.32 | 16.19 | 17.73 | 2.41 |
| x14 i_dec (round 2, the encoding before) | 17.72 | 11.70 | 9.13 | 19.07 | 16.68 | 14.73 | 16.40 | 2.17 |

Two times the lean tick on every loop but stores (9.62 against 9.82).

### Round 3 on x15 (x15m: the memory path)

`x15m.hlsl`'s switches: MOP2 (a tag is the word itself, an empty slot NONE; the texel's word picked by halves),
EXT2 (a byte's place `ad << 3`, the shift taking its low 5 bits; the merge as one xor and mask), HASH_PLAIN (the
word's low bits), WAYS1 (one way and a victim slot), MEM4 (a body a kind of bundle: integers, one memory op and lite,
two loads, a store and another memory op; 1, 2 and 3 are orders of the tests, 2 a switch), OVF (a store with no room
goes into one of two overflow slots that no lookup reads, and the pass ends after its bundle: no "bundle again").
On the card, `--fused 1`:

| Variant | Switches | arith | loads | stores | adds | tight | mix | stride | x lean (7) |
|---|---|---|---|---|---|---|---|---|---|
| x15 | | 20.46 | 12.09 | 9.56 | 22.01 | 19.27 | 16.14 | 15.86 | 2.36 |
| q_micro | MOP2 EXT2 HASH_PLAIN | 20.47 | 12.76 | 10.23 | 21.98 | 19.30 | 16.50 | 17.60 | 2.44 |
| q4 | MEM4 MOP2 EXT2 HASH_PLAIN | 20.72 | 12.74 | 11.27 | 22.14 | 19.58 | 16.48 | 16.94 | 2.48 |
| q4w1 | q4 WAYS1 | 20.75 | 14.57 | 11.72 | 22.23 | 19.56 | 16.40 | 16.90 | 2.54 |
| q4ovf | MEM4 MOP2 EXT2 OVF (two ways, x15's hash) | 21.01 | 12.61 | 11.70 | 22.62 | 19.78 | 17.60 | 18.01 | 2.55 |
| q4w1ovf | q4 WAYS1 OVF | 20.95 | 14.81 | 14.27 | 22.54 | 19.73 | 16.87 | 17.62 | 2.65 |
| q4o3 | q4w1ovf, the one-op body tested before the pairs | 20.59 | 14.67 | 13.34 | 22.12 | 19.39 | 18.48 | 18.86 | 2.66 |
| q4sw | q4w1ovf with a switch on the path | 15.74 | 13.38 | 11.72 | 16.90 | 14.79 | 14.94 | 16.22 | 2.18 |

q4ovf, q4w1ovf and q4o3 are the reference's on the card (every case, `--fused 1`). **q4ovf is two times the lean tick
on all seven integer and memory loops** (the least loads, 2.21; stores 2.38), with x15's two ways and hash; one way
(WAYS1) and the plain hash are faster on loads and stores but meet more conflicts on strided stores in real code. A
switch on the path is a quarter slower than tests in a row. Not kept from x14m: one lite op after the bodies, no-room
by selects (fxc copied registers in the shared head), one extend for either op. x14 and x15 count `cycle` wrong on a
store with no room (`budget = 1; cycle--` then `b0 - budget`): the timed loops never meet one; OVF counts it right.

### Round 4 on x15 (x15s, x15sq: the loops' structure)

`x15sq.hlsl` is x15m with the structure agent's switches: S_SW (a store bundle reads its texel only when an op needs
the old word: a byte or half store, or a load; =2 in the one-op bodies too), S_KIND (a whole step a kind of bundle,
chosen before it), S_ILOOP, S_STL, S_L2L (integer, store-pair and two-load bundles each in a do-while of their own,
its test one compare with the budget). Q is q4ovf's switches. On the card, `--fused 1`:

| Variant | arith | loads | stores | adds | tight | mix | stride | x lean (7) |
|---|---|---|---|---|---|---|---|---|
| q4ovf | 21.00 | 12.44 | 11.64 | 22.52 | 19.73 | 17.55 | 17.88 | 2.53 |
| sq_sw (Q, S_SW=1) | 20.88 | 12.69 | 12.04 | 22.46 | 19.65 | 17.28 | 17.78 | 2.54 |
| sq_sw2 (Q, S_SW=2) | 20.88 | 12.68 | 12.09 | 22.46 | 19.65 | 18.15 | 18.35 | 2.57 |
| sq_kind (Q, S_KIND) | 17.46 | 12.68 | 12.06 | 18.73 | 16.37 | 15.63 | 16.55 | 2.29 |
| sq_stlsw (S_KIND, S_STL, S_SW) | 17.12 | 12.94 | 12.78 | 18.44 | 16.14 | 15.69 | 16.62 | 2.31 |
| sq_ilsw (S_KIND, S_ILOOP, S_SW) | 19.59 | 12.84 | 11.29 | 21.10 | 18.41 | 15.49 | 16.22 | 2.38 |
| sq_loopssw (all the loops, S_SW) | 19.45 | 15.70 | 12.71 | 21.11 | 18.44 | 15.45 | 16.17 | 2.49 |
| s_ilstlsw (on x15) | 19.59 | 12.41 | 11.86 | 21.10 | 18.41 | 14.71 | 16.30 | 2.37 |

sq_sw2 is the reference's on the card (every case, `--fused 1`, and seed 7): a word store's merge needs no texel, and
not waiting for it is worth 4% on stores and 3% on mix. A step or a loop a kind of bundle costs the integer loops 7 to
17% again (as round 2's I_LOOPS did), though a loop of two-load bundles is 26% on loads. x14s (never timed): the loop
test as one compare with the budget saves 1 to 2 DXBC; a tail in each branch, a switch on the class and two steps a
trip do not pay.

## x16: the tick written plainly, RAM 8192 wide, the ROM read

x16 is sq_sw2 with no switch and no macro (`#include "../../isa.h"` its one line of the preprocessor; constants are
`static const uint`, the bodies plain code): 2,042 DXBC as sq_sw2 and as fast (2.57 both, in one run). The bench
reads a step's `WCN` and `RAM_W` from its source (`#define` or `static const uint`).

RAM's texture is 8192 texels wide (texel t at t % 8192, t / 8192; 256 MB is 8192 x 2048): no instruction more. The
ROM (GVM_ROM, 0x4000_0000 on) is a texture of its own, read only: every load reads RAM's and the ROM's texel at the
same place and the address's bit 30 chooses (+32 DXBC over the shader). The check case `rom` loads every width from
it beside RAM's loads and stores. On the card (`--fused 1`, four rounds):

| | arith | loads | stores | adds | tight | mix | stride | x lean (7) |
|---|---|---|---|---|---|---|---|---|
| x16 at 2048, no ROM | 20.84 | 12.66 | 11.97 | 22.38 | 19.62 | 18.10 | 18.38 | 2.56 |
| at 8192, no ROM | 20.86 | 12.68 | 12.08 | 22.42 | 19.62 | 18.14 | 18.23 | 2.57 |
| at 8192 and the ROM (x16 now) | 20.66 | 13.03 | 14.06 | 22.23 | 19.44 | 17.96 | 18.16 | 2.62 |

The ROM's read costs arith, mix and stride about 1%, and fxc2 schedules the loads and stores better beside it (stores
+16%): it stays. (Had it cost more, the ROM would have been a part of RAM: copied in by the harness, baked into the
RAM texture in Unity, and writable.) x16 is the reference's on the card in every case, `rom` among them.

## x17: the whole ISA (22 October 2026)

x17 (`x17.hlsl`, 800 lines, no switch) is x16's fast loop and everything else the reference makes: the general step
(every op of every class, op by op from pc's word: any address, d and q at any alignment, atomics and cas, the
operations region), traps and eret, the special registers, interrupts between bundles (the fast loop is not entered
while one is due, and none can become due inside it), wait (a supervisor's waits across passes, a user's ends the
pass), sync, the operations' queue (8 in flight a core) and the commit's operation rounds (stage, write, progress).
Whole bundles of paths 1 and 2 are x16's full step (`step12`). Many cores are rows. The state, the cbuffers and the
draws of a pass are `gpu_vm/tick/STATE.md`, the GPU harness's contract.

Two changes to the ISA came with it (`isa.h`, `gvm_ref.h`; gvm_cpu's 19 tests pass; the LLVM target, the runtimes and
the kernel are rebuilt for them):
- **A byte, a half or a word stays in its address's word**: a b, h, w, bu or hu access reads or writes only the bytes of
  its word (a load's missing bytes are 0, before its extension). What the fast loop always did; a test of a word
  crossed costs the memory loops 6 to 9% in any form (x17b `stk` 2.38 against `stknox` 2.60; `stku`, the test as
  integer arithmetic, 2.44). d and q are any alignment as before. The compiler splits what it does not know to be in
  one word.
- **The paths renumbered**: the memory paths 3-6 (load, two loads, store, store and another), the integer path 7.
  While an operation is in flight the fast loop's test is `path >= 7` against a register, `path >= 3` otherwise: memory
  bundles go to the general step, which stops at what an operation still holds, at no cost (x17b `stkthr`).

Also fixed in the reference: an access to the operations region at 0xfffffffc wrapped `a + bytes` and read or wrote
`op[]` far out of its bounds (now `a - GVM_OPS <= 32 - bytes`).

How the fast loop stays exact without a branch: a load outside RAM and the ROM (bit 30 off: `a & ~GVM_ROM >= RamEnd`)
or a store outside `[GVM_DEV, RamEnd)` (the boot area, the operations) is only noted, the largest address of each kind
in `lmax` and `smax` (one `max` an op). A pass that noted one leaves the state as it found it (flag 16, no store, its
operations' progress kept) and the next pass makes it again with every memory bundle in the general step, which traps
or latches as the reference. A pass is the same each time it is made, so this is exact; it costs a pass and a slow pass
a fault, which a program meets once. Faster ways tried and dropped, in x16 against x17 batches on the card (`--fused 1`,
x lean over the seven loops; x16 2.63 in every batch):

| Batch | What | x lean |
|---|---|---|
| x17a | a branch out of the fast loop before the memory bodies (`chk1`); by selects into sinks (`chk2`); in the body dispatch (`chk4`); the branch after the texel reads (`chk3`); no check at all (`chk0`, a probe) | 2.38, 2.16, 2.41, 2.42; 2.54-2.58 |
| x17b | a flag ORed, no branch, with the word crossed (`stk`) and without (`stknox`); the loop test with a register (`stkthr`) or `- 3 <= lim` (`stklim`) | 2.38, 2.60, 2.38, 2.36 |
| x17c | probes on x17's first form (a loop of two attempts round `run`): one attempt (`once`) 2.56, no general step (`nogen`) 2.47 as x17: the general step's code costs nothing, the loop round `run` did | 2.47-2.56 |
| x17d | the pass made again inside `run`'s loop (`restart`), or in the next pass (`redo`) | 2.49, 2.49 |
| x17e | the flag as bools ORed in each body (`flag0`), as the largest address of each kind (`flag1`, x17's) | 2.52, 2.56 |
| x17g | a word store off its word's start noted (made again) so that its texel is read by the opcode alone (`mis2`) | 2.48 |
| x17h | the state the fast loop does not use in an indexable array (`sya`) | 2.55 (as x17) |

On the card (`variants/x17f.txt`, four rounds; floats and fmix are not among the seven):

| | arith | loads | stores | adds | tight | mix | stride | x lean (7) | floats | fmix |
|---|---|---|---|---|---|---|---|---|---|---|
| x16 | 20.70 | 13.07 | 14.10 | 22.31 | 19.49 | 18.03 | 18.43 | 2.63 | 8.05 | 10.41 |
| x17 | 20.86 | 12.66 | 12.99 | 22.43 | 19.60 | 16.93 | 17.45 | 2.55 | 7.51 | 9.82 |
| x17, paths 1-2 op by op in the general step | 20.85 | 12.67 | 12.98 | 22.41 | 19.63 | 16.92 | 17.45 | 2.55 | 3.78 | 6.19 |

x17 against the lean tick: arith 2.53, loads 2.22, stores 2.65, adds 2.74, tight 2.69, mix 2.34, stride 2.74. What is
left of x17's 3% is the address keys and the texel read of a word store off its word's start (in each memory body).
HLSL makes both sides of `?:` too: `budget = c ? step12(budget) : general(budget)` ran both steps (an `if` now).
fxc2 took `(float)c`'s bits for `c` in a vertex shader's texture loads (the progress draw read row 0x3f800000 for core
1): a position is made from another integer expression.

Checked against the reference on WARP and on the card, every case, seeds 1, 7 and 99 (`--fused 1`) and 3 (without):
x16's and the new ones, `memx` (d and q at any alignment, b/h/w off their alignment, atomics and cas, d and q from the
ROM, faults past RAM's end, at 0xfffffffc, in the boot area and of an atomic off its word, the operations region read by
d and q and latched), `irq` (TIME 1000 + the pass in both machines: the timer, a supervisor's wait woken by it, the
software interrupt, the external line in passes 50-51, every special register, xsr, srset, srclr, then user mode: its
privileged ops and eret trap, its wait ends the pass, a syscall) and `opq` (two cores with operations in flight and no
sync: a load of a destination and a store to a source wait, nine asked at once, the counts by ld.d, the latch by ld.q, a
blit whose rows overlap, a copy backwards, a refused one, a copy of 640 KB over three passes). `--op-rounds 1` and `4`
too. `GVM_DEBUG=1` prints each core's EPC, CAUSE, queue and progress texels after a check.

### x17's write cache under the kernel's boot

Linux's boot on the card (`gvm_gpu --stats --until "comes back"`, one core) ended 14,798 of its 14,847 passes on a full
write cache, at 414 bundles a pass: streams of stores (memset and copy loops of `sw` and `sq`, the clearing at its start
two bundles a word) fill it by capacity, not by its sets' conflicts (the bundles a pass grow with the entries). Entries
(two ways of HALF sets) against the boot's time to init's prompt and the bench (`variants/x17w.txt`):

| WCN | state pixels a core | boot passes | boot | M bundles/s | x lean (7) |
|---|---|---|---|---|---|
| 130 | 14 | 14,847 | 2.10 s | 2.92 | 2.55 |
| 258 | 22 | 7,423 | 1.84 s | 3.21 | 2.54 |
| 514 (x17 now) | 38 | 3,935 | 1.71 s | 3.38 | 2.55 |
| 1026 | 70 | 1,951 | 1.61 s | 3.57 | |

514: every pixel of a core's row runs the core, so more state is more lanes a core; 1026 gains 6% more for 70.
The boot's bundles by path (gvm_cpu's trace, 6.1 M bundles): 7 (integers) 47%, 3-6 (memory) 33%, 1 10% (`step12`), 0
10% (mostly bundles whose control word is cc 0: calls, returns, the control word's add, made op by op by the general
step).

## x18: real code's bundles cheaper (22 October 2026)

On real code a bundle cost more than on the bench's loops: CoreMark 174 ns, gzip 198, gunzip 210 on the card under Linux
(`gpu_vm/bench_real`), the lean tick 153-174 an instruction. The bench now times real programs itself
(`gvmbench --case coremark,coremark_pie,gzip,gunzip`, in ns a bundle; `gpu_vm/tick/real/build.sh` makes the ELFs in
`build/tick_real`: CoreMark at -O2, `gzloop.c` gzip or gunzip over bench_real's input; a check runs a short piece of
each against the reference), and probes of one kind of bundle (`p_int`, `p_load`, `p_store`, `p_sel`, `p_call`,
`p_scat`, `p_page`). `GVM_BPP=1` prints a ns case's bundles a pass; `gpu_vm/tick/real/mix.py` a program's mix from
gvm_cpu's trace. Every variant is checked on WARP on every case before it runs on the card (the lead's rule since a
GPU fault on 22 October), and every loop of x17 and x18 is capped (a guard on the pass's trips, the queue's counts
clamped: free, `x18t`).

Where real code's time goes (x17, the card at full clock):

| kind of bundle | ns | CoreMark's share | gzip's |
|---|---|---|---|
| integer (path 7) | 103 | 54% | 52% |
| one load or store and lite (3, 5) | 185-187 | 39% | 31% |
| pairs (4, 6) | ~180 | 3.5% | 15% |
| path 0: calls and returns (cc 0) | ~330-440 each, the fast loop left | 2.0% | 0.6% |
| path 0: sel | 416 | 1.8% | 0.5% |

Nsight (`trace.ps1 -Dxbc`, now with fxc2's code; `GPUTRACE_FRAME.xls` has the warp states) on CoreMark: issuing 39%,
fixed-latency waits 30%, texture and local memory 13%, branches 9.5%, no instruction 3.5% - as the loops (p_int: 43,
31, 2, 14): real code is not memory bound; its cost is the mix (memory bodies, the general step) and the instructions.
The write cache's size (x18b: 130-1026 entries) moves CoreMark by 2%; passes end early only for scattered stores
(gzip ~2,400 bundles a pass, a pass's fixed cost ~21 us of draws and state).

x18 is x17 with two changes:
- **The fast loop's order**: `path != 7` first (integers fall to the else), then the one-op memory body (3 and 5), two
  loads, then a store pair; x17 tested the integer path, then the pairs, then the one-op body (`o4`).
- **step12 makes cc 0's bundles, sel and lpc** (an ISA change of paths alone, `isa.h`, `gvm_asm.h`): a cc 0 bundle (jr,
  callr, call, a far j, the add) takes path 1 when its ops are path 1's or 7's, path 2 when one load or store and lite or
  float lite, else 0 (eret stays 0); path 1's op 0 may be sel or lpc. step12's op 1 is lite or float lite by its class.
  jr and callr still read rs after the ops. Old binaries run unchanged (those bundles take the general step).

On the card (`variants/x18o.txt`, the clock at ~2.4-2.8 GHz; x18 is o4s3; x17 on the programs as compiled):

| | arith | loads | stores | adds | tight | mix | stride | floats | fmix | CoreMark ns | PIE CoreMark ns | gzip ns | gunzip ns |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| x17 | 20.48 | 12.49 | 12.78 | 22.04 | 19.11 | 16.72 | 15.83 | 7.38 | 9.73 | 172.8 | 188.0 | 207.0 | 165.4 |
| x18 | 20.19 | 12.78 | 11.45 | 21.38 | 19.00 | 17.98 | 17.30 | 7.07 | 9.42 | 149.7 | 154.9 | 206.9 | 161.2 |

CoreMark -13%, PIE CoreMark -18%, gunzip -3%, gzip as x17 (`variants/x18.txt`, the card at ~2.0 GHz: CoreMark 197 ->
170, PIE 206 -> 181, gzip 256 -> 246, gunzip 191 -> 183). Every loop at 2x the lean tick or more (the least loads,
2.24). The target, 138 ns on CoreMark, is not reached: 150.

Tried and not kept (x18v's switches, batches `variants/x18a-t.txt`; the card's clock moved between 1.1 and 2.8 GHz this
afternoon, so only a batch's own rows compare):
- Code or data from a StructuredBuffer instead of RAM's texture (x18a): integers 2% slower, loads 5% faster.
- cc 0 in the fast loop: the jr's target selected before the fetch (`CC0=1`, a field layout moved), by a branch after
  the ops (`CC0F=1`), or fetched beside the two from rs as found (`CC0F=2`): the seven loops 22-40% slower. Anything
  added to the fast loop's control path costs every bundle far beyond its instructions.
- sel as op 0 of the integer path (`SELF`): the loops 5-15% slower. lpc as integer I form 24 in the fast loop (`LPCF`):
  2.5% on the loops, CoreMark +3%.
- step12 with every integer op (shadd, the carries, bfe) and mulh-brev (`S12X=1`): as x18 on CoreMark, floats 9% slower.
- step12 fetching both next bundles first (`S12P`): slower; a call or return costs its exit from the fast loop.
- The write cache as two arrays of words (`WCSPLIT`), four ways (`WAYS=4`: loads -22%), a multiplicative hash
  (`HASH`), the second way hashed by the word's 256-word block (`SKEW`: stores a pass on random words +58%, gzip's
  bundles a pass +83%, but the loops -7% and CoreMark +6%); the ROM in RAM's texture (`ROMROWS`: slower); loads apart
  from stores (`ORD` 2, 3, 5, 6); the one-op body's texel read always (`TEX1`); fxc2's other flags (`variants/x18r.txt`).

## x19: the per-bundle cost and the write cache (22 October 2026; no new default)

Goal: real work at 2-3x the lean tick (x18: CoreMark 2.11x, gzip 2.02x, gunzip 1.62x under Linux). Nothing below beat x18
clearly, so **x18 stays the default**; the variants are kept as copies (`steps/x19*.hlsl`, `variants/x19a-v.txt`) and no
ISA change was made (the probes that needed one used a reference flag that is not kept).

New in the bench (`gvmbench`, `cases.h`): `--cores N` and the many-core cases `mc_int`, `mc_load` (a probe's code on every
core), `mc_cm` (CoreMark copies linked apart, `build/tick_real/mc`, `real/build.sh`), `mc_cm1` (one copy shared, as
threads), `mc_mix` (CoreMark, gzip, CoreMark, gunzip on neighbouring cores), timed in ns a bundle of all cores together;
probes `p_nop`, `p_int2`, `p_dep2`; the check case `wcov` (random stores in long passes, `Case::check_ticks`);
`GVM_ENDS=1|2` (why core 0's passes end, bundles and entries used a pass); `GVM_CHECK_SECONDS`; steps with `LROWS`,
`fragu` (UAV state), `op_any_vert` (predicated rounds), `WCN1` and `WORKER` are understood. `real/memsim.cpp`: a real
case on the reference with models of the write cache, a texel cache and the paths (WSL: see its head).

What was measured (card, batches against x18 in one run; ns a bundle; x18: p_int 105.6, p_load 167, CoreMark 152,
gzip 207, gunzip 172):

| Variant | What | Result |
|---|---|---|
| `p_nop`, `p_int`, `p_int2`, `p_dep2` | a bundle of 0, 1, 2 integer ops, op 1 on op 0's result | all 105.7: the ops cost nothing over a bundle's fixed work |
| x19v TC=1, 2 | a load's texel from the last 1 or 2 texels in registers (RAM is constant in a pass) | real code as x18 (2-entry: p_load 159): texture latency is not the memory bundle's cost |
| x19v PF | the fall-through's bundle fetched an iteration ahead | +15% everywhere (with TC=2: as x18) |
| x19v HOIST=1, 2 | op 0's (and op 1's) texel fetched before the path's branch | +13% integer bundles, memory no faster |
| x19v BAO=1, 2 | the branch compares after the ops (forwarded; or registers read again) | +7.5% and +13% a bundle; the LLVM agent's 1.5-9% fewer bundles do not pay for it |
| x19v LG, LATESEL, NOPAIR | one compare for budget and cap; the next bundle's select after the ops; pairs out of the fast loop | +2%, +1-3%, +4% |
| x19u | one lane a core, its state written as UAVs | +13% a bundle; many cores alike |
| x19l LROWS 2, 4, 8 | a core's state pixels on several rows | one core +0, +14, +14%; mc_mix (32 cores) -2, -4, -15% |
| x19w | the write cache a set a texel (one local read a lookup) | loads loop +7%, CoreMark +3%, gzip -2% |
| x19o | an overflow list of 30 entries flagged on way 1's tag | gzip passes 1,000 -> 6,190 bundles, but every bundle +13-40% |
| x19v + op_any | the operations' rounds predicated on a core having one (D3D11 predicate) | no change: the rounds' cost is their render target switches, not their draws |
| x19w130/258/1026/2050 | 130, 258, 1,026, 2,050 entries (14, 22, 70, 134 state pixels) | below |
| x19g, x19h | core 0 1,026 entries, workers 130 on 14 pixels (a set's two ways a state texel; x19h compiles a workers' tick apart, `WORKER=1`) | x19g: CoreMark +4%; x19h: one core as 1,026, but its two tick draws run one after the other (many cores -40%) |

The write cache's size (one core: ns a bundle; many: ns a bundle of all cores, `mc_cm1`, CoreMark on every core):

| entries | CoreMark | gzip | gunzip | p_scat | 64 cores mc_int | 128 cores mc_cm1 |
|---|---|---|---|---|---|---|
| 130 | 147.3 | 218.3 | 160.4 | 283 | 1.71 | 1.46 |
| 258 | 151.1 | 213.8 | 168.9 | 208 | 1.73 | 1.63 |
| 514 (x18) | 152.0 | 207.5 | 171.6 | 169-175 | 1.82-1.94 | 1.78-1.86 |
| 1,026 | 152.4-153.0 | 197.4-199.5 | 173.1 | 157-164 | 2.77-2.85 | 2.83-2.85 |

Why passes end early: a 2-way set takes a third word only on the next pass, so scattered stores end a pass after about
80 words (15% of 514 entries): gzip's passes 1,000 bundles in the bench, a shell loop's 913 (`gvm_gpu --trace-pass`, every
pass "full"). At 1,026 entries: 2,110 and 1,675. A pass's fixed cost in the bench is about 20 us (tick 12, two operation
rounds 8; one round 6 us less). Under `gvm_gpu` (Linux, batches of 32) the pass count does not show in the wall time (the
bench's gzip and gunzip under Linux alike with 514 and 1,026). In `rvc_harness --gvm` (a pass a frame) it does: a shell
loop of 2,000 steps 8.4-8.9 s with x18, 6.7-8.0 s with 1,026 entries; mctest 255 alike; nxray with 255 workers 165 ms a
picture with x18, 190-207 with 1,026 (more state pixels and commit points a core). So 1,026 trades many cores for one;
not taken as the default.

What is known about the cost now: the dependent latencies on this card (`gpu_vm/tick/probes/lat.hlsl`, `lat.cpp`, one pixel) are an
integer op about 0.6-0.9 ns, a local-memory load 6.4, a store then a load 7.1, a texel 45; a single warp issues 32
independent ops in 6-7 ns. A bundle (about 150 DXBC) takes 105 ns: neither the issue rate nor any one latency chain
explains it, and every change to the fast loop's shape moves it by a few percent either way. The driver's own numbers
(Vulkan's pipeline statistics on DXC's SPIR-V of x18, `gpu_vm/tick/probes/vkstats.cpp`): 191 registers, 4.4 KB of local memory
(the arrays, no spills), 91 KB of code. Kernel bundles are 8.5% of Linux gunzip's (gvm_cpu trace), so its 1.6x is the
harness and user code, not the kernel.

Many cores: in gvmbench lockstep cores scale nearly linearly to 64 (p_int 8 cores 7.9x, CoreMark one copy 32 cores
30x); different programs on neighbouring rows (quads hold two cores) or copies at different addresses cost 1.5-2x.
