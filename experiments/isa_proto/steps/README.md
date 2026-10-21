# The tick built up from one instruction

Each step is a shader of its own, run by `bin\isabench.exe --shader experiments\isa_proto\steps\sN.hlsl`
on hand-assembled loops and checked against the host's reference (registers, pc, RAM). Timed on the RTX
5090 (D3D11, fxc2), 8,192 instructions a pass, through `tools\gpu_queue.ps1`, variants A B ... then reversed.
Nanoseconds an instruction; the lean tick runs `factor` at 8.48 million a second here (118 ns), and the same
loops as RISC-V at the speeds in "Where it stands" (`../lean/isaloops.c`, below).

| Step | What it adds | Switches |
|---|---|---|
| s1 | one op: `r[rd] = r[ra] + r[rb] + imm`, a branch on equality | `CODE_SRC`, `FETCH_AHEAD`, `REG_SEL`, `BYPASS`, `ARR`, probes |
| s2 | a word load | |
| s3 | a word store, the simplest write cache (direct-mapped pairs) | `WCN`, `WC_CLEAR` |
| s4 | two sums a code texel (`--bundle`) | `CODE_SRC`, `FETCH_AHEAD`, `REG_SEL` |
| s5 | two step-1 slots a bundle (`--pairs`; `--par`: side by side) | `PAR` |
| s6 | s5 with the whole integer unit and s3's memory (`--halfpc`) | `ALU_SUM`, `NO_MEM`, `WCN` |
| s7 | an integer op and a memory op a slot, all done always (`--vliw`) | `ALU_SUM` |
| s8 | one op a trip, the integer unit, memory behind one branch, no break | `ALU_SUM`, `NO_MEM`, `ARR` |
| s9 | step 1 run ahead of its branch: static prediction (bit 10), a wrong slot's op dropped a trip later | `ARR` |
| s10 | K step-1 slots a trip, fetched together (`--group K`); `FWD`: reads first, results passed by selects | `GROUP`, `FWD`, probes |
| s11 | groups with the whole unit and memory in every place, ops decoded ahead into two words (`--decoded K`) | `GROUP`, `ARR` |
| s12 | groups of four places with roles: integer unit, sum, sum or memory, sum or branch (`--roles`) | `ARR`, `WCN` |
| s13 | s8's op and a sum beside it, before or after (`--fuse`) | |
| s14 | the sum after only, fields where one instruction takes them, a branch on its own sum (`--after`) | `SEC_EARLY`, `SEC_BRANCH`, `NO_SEC` |
| s15 | the second slot a sum, a load or the next word's store; the loop's step and branch as one op (`--wide`) | probes |
| s16 | the second slot with its own integer unit (`--wide2`) | `NSCALAR` |
| s17 | s16 for a scheduler over each block (`--sched`); the second slot may take the op's immediate | `ARR`, `WCD` |
| s18 | s17 with a branch that steps its register and an add-then-mask second slot (`--sched2`) | `CODE_TEX` |
| s19 | s18's ops as one or two 32-bit words in R32 RAM, the next two words fetched ahead (`--ram`); not kept | |
| s20 | s18's slots in one RAM of 16-byte texels with the data (`--ram4`), branches relative, a count of 0 illegal | `WCD`, `CODE_COPY` |
| s21 | s20 with s18's write cache of words, committed in four draws, a component each (`--wordwc`) | `WCD` |

## What costs what

- **The fetch.** One sum a trip with 4 registers: code in a texture 68 ns, a structured buffer 51, a
  constant buffer 44; with 4 registers held as values and the next slot fetched ahead, 37. The loop
  alone is 2.8 ns, the fetch and pc alone 44.
- **The register count**, in step 1 with an array: 4 to 64 registers 68 to 71 ns (8: 84). In a larger
  shader it matters: s6 and s8 with 32 registers are a third slower than with 16.
- **Where the driver keeps the array.** Its declared size decides it, not the registers used: step 1
  with a bypass (the last result written after the next reads) is 152 ns with 16 and 46 with 64
  declared; s8's ALU ops 116 to 87 with 64 declared, its loads 171 to 252 and stores 167 to 324.
- **Branches.** In step 1 (44 ns) a branch never taken costs 12 ns when its body writes `r[rd]` as
  the op does, 136 when it writes a fixed register; a `break` never taken costs 72.
- **Two ops a fetch.** 31 to 35 ns an op with 4 to 16 registers (s4, s5); two independent ops side by
  side are no faster than one after the other (32 against 35).
- **Doing memory always** (s7): about 250 ns an op, a RAM load on every op's path.
- **The whole integer unit** as selects: about 20 ns an op over a sum alone (s6, s8).
- **The write cache**: 16 pairs against 32 cost 4 to 5% of every op and stores twice that (s6).
- **Prediction** (s9) gains nothing: 49 ns against step 1's 44; a branch's outcome is not the chain.
- **Groups** (s10): 2, 4, 8 slots a trip 41, 33, 42 ns an op; with reads first and results forwarded 36,
  32 (16 declared), 42. With no register array at all a group of 8 is still 35.5: the loop body is 28
  bytecode instructions an op, and a trip costs about 1.3 ns an instruction whatever the shape.
- **Pixels a core**: 5 against 35 (one warp against two) changes nothing.
- **Both arrays large** (s8): 64 registers declared and 64 pairs of write cache, arith 8.9 to 14.3, loads
  6.0 to 7.9, stores 6.2 to 8.0, `mix` 7.3 to 10.0; 128 of either changes nothing, 32 registers halves it.
- **Every place a whole op** (s11): 91 bytecode instructions an op, 100 to 150 ns. **Places with roles**
  (s12): a full group 255 ns, `mix` 8.1 (in-order packing leaves places empty).
- **The ALU as masks** (`ALU_MASK`): 96 instructions against 95; fxc follows each comparison with a
  `movc -1, 0`, so flags in one control word cost about 40 instructions however they are written.
- **A second op a trip** (s13 to s16): about 10 ns over s8's 70 when its operands are read with the op's and
  the op's result is passed to it by selects (s14 `SEC_EARLY`, 5 to 8% over reading them after the write);
  a trip of two costs 78 to 87 ns, of two loads about 145, of two stores about 180. s13's 51 more bytecode
  instructions (both orders, a 64-bit taken mask) made a trip 178 ns.
- **What did not move a trip**: the second write first or its index not waiting on the stop, equality alone for
  the compare, the next op's target fetched a trip ahead (s15 probes, all level or slower); the loop's count in
  r1 or r7. A branch on its own sum (s14) is as slow as two trips: the add lies on the chain to the next op.
- **The scheduler** (s17): pairs over each block's dependences, the fewest trips by search over what is done;
  a slot whose ops before it are a prefix of the block is marked, and a pass ends anywhere (the machine's state
  is consistent; the bench lands on a marked slot with passes of one before its check). Ending passes only at
  marked slots in the shader cost 12% of every trip. Declared sizes: 64 or 128 registers alike, 32 a third.
- **Where code is fetched from** (s18, s19): s18's slots from a texture of 16-byte texels (`CODE_TEX`) are as
  fast as from the constant buffer, also when that texture is a render target; 32-bit words in R32 RAM (s19)
  made every trip 2.7 times as slow (four loads a trip, a variable length decoded on the chain). So RAM is
  16-byte texels, code a slot a texel in it (s20).
- **Helper lanes.** A core's row is one pixel high, so the quad's second row runs too, reads its state out of
  bounds as 0 and starts at pc 0. In s20 that is zeroed RAM: it walked 131,072 empty texels to the code every
  pass (21 ms a pass on the card, 90 on WARP). A count of 0 is illegal now and ends the pass.
- **A write cache of whole texels** (s20): a `uint4` array indexed at run time made loads 13.0 million a second to
  2.6 and stores 11.4 to 2.4 (4.4 and 3.9 declared 32). Words kept as s18 keeps them and the commit drawn four
  times with a component's write mask each (s21) cost nothing: as s18 on every loop.

## Where it stands

The lean tick (`rvc_lean`, D3D11) runs the same loops written as RISC-V (`../lean/isaloops.c`, timed by the
guest's clock, second of two rounds): built with the no-MMU `rv32-cc -O2`, made flat with `linux/nommu/flat.py`,
put in a private image and run as `isaloops 16` under `rvc_harness --d3d11 --rvc experiments\rvc_lean`.
`mix` and `stride` there add a base and an index apart (12 and 7 instructions); here a load takes both.

| | arith | loads | stores | adds | mix | tight | stride |
|---|---|---|---|---|---|---|---|
| lean tick, measured | 8.42 | 5.55 | 4.75 | 8.30 | 7.24 | 7.34 | 6.25 |
| s8, one op a trip | 14.5 | 8.2 | 8.1 | 14.5 | 12.7 | 14.4 | 9.8 |
| s16, a second slot with its own unit | 22.4 | 12.8 | 10.5 | 23.6 | 15.3 | 24.4 | 11.7 |
| s17, scheduled | 21.6 | 12.6 | 11.5 | 22.6 | 17.1 | 23.7 | 12.6 |
| s18, scheduled with the two fused forms | 23.2 | 12.6 | 10.4 | 21.8 | 16.0 | 23.1 | 15.8 |
| s18 against the lean tick | 2.76 | 2.26 | 2.18 | 2.62 | 2.22 | 3.15 | 2.53 |
| s21, code and data in one RAM of texels | 23.9 | 13.4 | 11.0 | 22.9 | 19.4 | 22.8 | 16.8 |
| s21 against the lean tick | 2.84 | 2.42 | 2.32 | 2.76 | 2.68 | 3.11 | 2.68 |

Millions of RISC-V instructions a second (the loop's work: a slot that does two counts two). Trips an
iteration in s18: arith 8 for 17, loads and stores 8 for 16, adds 8, mix 5 for 12, tight 1 for 2, stride 2 for
7. Sums alone reached 3.8 times the lean tick (s10); the whole integer unit and memory one op a trip 1.7 at
most (s8). A trip costs what its longest chain costs, not its instruction count: two ops a trip are cheap
when the second reads nothing the first has not yet given it. Stores and `mix` lost 6 to 10% from s17 to s18
to the two selects the fused forms add to every trip.
