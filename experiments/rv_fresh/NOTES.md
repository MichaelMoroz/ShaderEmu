# A tick grown from an empty shader

Not a copy of `rvc_lean` or `w3*`: each piece is added and measured on the card before the next. Target: about 139 ns per RV
instruction at about 8,000 a pass, real code, with the write cache not filling early.

`bench.cpp` (built by `CMakeLists.txt` to `bin\fresh.exe`) draws a block of pixels over RAM in a texture and eight state targets, and
times the draw alone (D3D11, median of batches of 32 passes): there is no commit, no devices and no sleeping workers in these figures.
`run.py` runs a step through `tools\gpu_queue.ps1` and adds a line to `steps.log`; `batch.ps1` runs several; `progs.py` has the test
programs; `sim.py` is a model to check registers against. `tick.hlsl` is step 1, `steps\` the later ones.

## The steps (RTX 5070 Laptop, 8,192 instructions a pass, two runs each, the median)

| Step | What came in | ns an instruction | Cost |
|---|---|---|---|
| 1 | Loop, a texture fetch every instruction, state in and out | 24.3 | |
| 1 | The same, core 8 x 8 | 24.4 | none |
| 2 | RV's fields decoded, no register file | 29.8 | +5.5 |
| 2 | Register file: two reads, one write, `x[rd] = x[rs1] + x[rs2]` | 42.5 | +12.7 |
| 2 | The texel kept in a rotating vector, a fetch only for a new one | 45.7 | +3.2: slower than fetching each time |
| 2 | That, core 8 x 8 / 8 x 4 | 48.4 / 45.7 | +2.7 for two warps |
| 3b L0 | Step 2's add again (fetch every instruction) | 42.4 | |
| 3b L1 | Second operand chosen, register or immediate | 47.3 | +4.9 |
| 3b L2 | Add, xor, or, and by bits of f3 | 46.7 | none beyond L1 |
| 3b L3 | And the shifts | 48.6 | +1.9 |
| 3b L4 | And sub, slt and sltu: all arithmetic | 97.0 | **+48.4** |
| 3 | All arithmetic and control, a branch between them (`TREE`), on `add14` / `alu` | 150.1 / 149.8 | +53 on L4 |
| 3 | All arithmetic and control on one path, no branch | 166.0 / 165.5 | +16 on `TREE` |

Step 3's registers on `alu` after 1,114,112 instructions equal `sim.py`'s, all 32 of them (at budget 2,048). Steps 1, 2 and 3b ran
programs whose registers stay zero (`adds` has `addi` decoded as an add), so their data never changes; 3b has not been checked against
the model.

Lean's own prices for comparison, whole frames with the commit and devices (about 20 ns an instruction of them), on
`acct/classes_s.c`, two runs each: add 113-120, arithmetic 125-130, load 169, store 197-204, a two-instruction loop 133-136. The
figure of 55 ns for gpu_vm's add is from its earlier results: this session's `gvm --case` run printed no prices.

The 97 and the 150 and 166 of the table above are not what they seemed: see below.

## The bisect (1 November 2026, card to itself)

- **L4's +48 ns did not reproduce.** All arithmetic is 50.9, 50.8 and 50.7 ns now, from the same code; each piece alone adds 3 ns or less
  (`LEVEL` 5 to 10). Why that one run read 97 is not known.
- **The cliff: a register array under 40 elements in a large loop.** With 33 elements, plain operations cost about 1 ns each until the loop
  is about 40 longer (89.8 ns), then 173.9 at 48: +84 in one step. It goes by code present, not run (64 unrun operations: 135 ns). With no
  array, or with 40, 48, 56, 64 or 256 elements, there is no cliff (the same loop 114 ns against 191). Code after the loop costs nothing.
  What the driver does there is not known. Every step since has 64 elements or more. Lean's array has 65.
- Step 3 at 64 elements: 88 ns with a branch between arithmetic and control, 95 on one path, registers as the model's.

## The steps since (64 elements or more; the draw alone unless it says whole)

| Step | What came in | Program | ns an instruction |
|---|---|---|---|
| 5 | One plain loop, all arithmetic | `adds` | 50.4 |
| 5 | An outer loop, `if (...) break` as the way out | `adds` | 94.9 |
| 5 | An outer loop, the next word fetched at a trip's end and asked in the loop's own condition (gpu_vm's shape) | `adds` | 62.6 |
| 6 | Control as a way out to the outer loop | `alu`, `jmp6`, `jmp4`, `blt` (2 in 5) | 64, 72.5, 78, 87 |
| 6 | Control on the hot path, by selects | the same | 113 on all |
| 7 | A load in the outer loop, no write cache | `alu`, `loads`, `mix0` | 65, 153, 101 |
| 7 | A loop of its own for loads | the same | 66, 97, 108 |
| 7 | The load behind a `[branch]` in the hot loop | the same | 101, 136, 120 |
| 8 | Stores, write cache of 368 whole texels by probing, a 64-bit filter, the commit | `alu`, `loads`, `stores`, `mix`, `mix2` | 67, 178, 170, 157, 154 |
| 9 | The load on the hot path by selects, the texture read on every trip | the same | 116, 117, 178, 207, 215: rejected |
| 10 | Buckets of four by selects in place of probing | the same | 77, 179, 216, 153, 164: not better |
| 11 | Step 8, RAM read first, one branch to the table | the same | 67, 163, 169, 154, 161 |

A never-taken `if` costs more the further it jumps: about 8, 24 and 45 ns over 8, 32 and 256 operations (no array), but 8.5, 12 and 42 over
8, 64 and 256 at 64 elements, so this is not a plain line in distance.

Steps 6 to 11's registers equal `sim.py`'s where checked (`mix0` on step 7, `mix` on step 8, `mix2` on steps 9, 10 and 11; memory itself is
not compared). `fill` on step 10: the cache stopped a pass at 292 stores of 368 entries (79%); step 8's probing has not been measured so.

**Against lean, the same programs, whole frames** (`lean_on.py`, one run each). Lean's has the commit and devices; the fresh tick's has its
commit and no devices, traps, atomics, multiplication, floats, unaligned accesses or sleeping workers:

| Program | Lean | Step 8 | Step 11 |
|---|---|---|---|
| `alu` | 126.3 | 69.8 | 68.9 |
| `loads` | 175.2 | 190.2 | 168.8 |
| `stores` | 205.7 | 175.4 | 175.6 |
| `mix` | 168.1 | 167.6 | 163.1 |
| `mix2` | 159.9 | 160.2 | 171.8 |

## What it says so far

- Arithmetic is about half lean's price. The mixes, half of whose instructions leave the hot loop, are level with lean, and the fresh tick
  still lacks what lean carries. So on these figures it is not yet ahead where it counts, and the target (139 ns on real code) is not shown
  reached or out of reach.
- What the fresh tick pays is the way out: by the mixes' figures more than the kinds' tight loops add up to. Why branches and ways out cost
  what they do is the open question, and the figures above (a jump's price not a line in distance) cannot settle it. A SASS listing would.
- How often real code leaves the hot loop decides this design. The mixes are made up: 6 of 12 arithmetic. No real program has run.

## Where it stopped

Next: real code without the whole machine (a snapshot's RAM and registers, on to the first instruction the tick lacks), which needs
multiplication and more; the path out cheaper, with SASS if a trace can be had; the translated form as a step of its own (not begun). The
head-to-head against gpu_vm (shell, gzip, Doom, Quake; flat, World, VR) has not been run again, and nothing here has had a WARP check.
