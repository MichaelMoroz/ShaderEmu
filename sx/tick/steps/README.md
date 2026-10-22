# SX's tick built up in steps

Each step is a shader of its own (`xN.hlsl`), run by `bin\sxbench.exe --shader sx\tick\steps\xN.hlsl` on the
cases it can make and checked against the reference machine (`sx/ref/sx_ref.h`: registers, pc, the count of
ops, every word of RAM). Built with `sx\tick\build.bat` (MSVC; the compiler beside the exe is fxc2,
`docs/machine/fxc2.md`), timed on the RTX 5070 Laptop through `tools\gpu_queue.ps1`, 1,000 passes of 8,192
bundles, one core. The timed loops are `experiments/isa_proto/lean/isaloops.c`'s written in SX, scored in that
file's RISC-V instructions a second (the lean tick's measure).

The start is `experiments/isa_proto/steps/s21.hlsl`, the last verified step of the prototype; the target is two
to three times the lean tick on the same loops.

## The two machines it is held against (this card, 22 October 2026)

Millions of RISC-V instructions a second. The lean tick: `isaloops 16` in a private no-MMU image
(`build/images/sxlean` of the main checkout, the prebuilt root and the program built by `~/nm/work/bin/rv32-cc
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
| x1 | SX's bundle: three slots of a sum (add, addi), the control word's branches and `j`; reads first, results to later ops by selects, writes last | 140 | | 23.1 | 14.4 |
| x2 | the integer unit's 24 functions in every slot (`isa.h`'s numbering: a tree of selects) | 255 | 11.1 | 12.6 | 7.8 |
| x3 | x2 with a body for each count of ops (1, 2, 3), chosen once a bundle | 187 to 510 | 5.6 | 10.1 | 10.7 |
| x4 | slot 0 the whole unit, slots 1 and 2 sums, logic and the three shifts (f 0-10); the assembler opens a bundle for an op that would land in a slot without its function | 220 | 12.9 | 14.5 | 9.1 |
| x5 | x4 told by the control word: L bits mark the long immediates, the path says which body runs the bundle (no op looked at to decide) | 200 | 14.1 | 16.1 | 10.1 |
| x6 | two op slots (word 2 the long immediate of the op the control word names), the branch on the registers as the bundle found them, r0's writes into r64 | 125 | 16.9 | 15.9 | 16.0 |

Against the lean tick, x5 is 1.80 (arith), 2.07 (adds) and 1.46 (tight); x6 2.16, 2.06 and 2.32.

From x6 on SX has two op slots, and its branches compare the registers as the bundle found them (decided 22
October 2026 on these numbers: `sx/isa.h`).

| Step | Memory | ns: an integer bundle, a load's, two loads' | arith | loads | stores | mix | stride | tight |
|---|---|---|---|---|---|---|---|---|
| x7 | path 2: op 0 a load or store (b, h, w, aligned, in RAM), op 1 a cheap integer op; a write cache of word pairs, two ways of 64; the commit (four draws) | 136, 216, | 15.5 | 5.30 | 4.33 | 12.8 | 11.9 | 14.8 |
| x8 | path 3: both ops loads or stores; op 1's store with no room undoes op 0 | 169, , 313 | 12.5 | 6.39 | 4.87 | 10.7 | 10.0 | 11.8 |
| x9 | one `mem()` for both ops, a loop of one or two trips (x8's three copies made the loop 779 DXBC instructions; this 207) | 152, , 394 | 14.0 | 5.08 | 3.90 | 10.2 | 9.8 | 13.2 |
| x10 | no `break` in the loop (a stop zeroes the budget) | 158, , 400 | 13.5 | 5.01 | 4.14 | 10.1 | 9.7 | 12.9 |
| x11 | loads as s21 makes them: both ops' addresses, texels and cache entries read at once, words chosen by selects (stores halt) | 165, , 222 | 12.6 | 8.99 | | | 10.6 | 12.4 |

The lean tick's loads 5.49, stores 4.63, mix 6.85, stride 6.04: x11 is 1.64 on loads and 1.76 on stride, and its
integer bundles 1.6. s21's trip of two loads is 166 ns against its integer trip's 103; x11's 222 against 165.

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
