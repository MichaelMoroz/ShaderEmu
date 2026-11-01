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

## Real code (1 November 2026)

`snapsim.py` runs a snapshot's core 0 on a model, from its RAM and registers, to the first thing the tick lacks. `real.py` runs the same on the
card (`bench --snap`), repeating the stretch; the first go is not timed and only changed texels are put back between goes. Step 12 adds RAM at its
addresses, multiplication and division, atomics, fence and the time CSR read as 0; anything else stops it.

**How often real code leaves the hot loop (the model):**

| Load | Stretch, instructions | Stops at | Arithmetic | Load | Branch | Store | Other | Leaves the loop |
|---|---|---|---|---|---|---|---|---|
| shell | 6,086 | `ecall` | 40.4% | 22.2% | 17.6% | 11.4% | 8.4% | every 1.70 (58.8%) |
| Doom | 29,478 | a load from the devices (`81000c00`) | 35.2% | 27.8% | 20.2% | 8.1% | 8.7% | every 1.56 (64.0%) |
| Quake | 21 | a load from the devices (`80c01404`) | | | | | | too short to say |
| nxray | 0 | a float instruction | | | | | | none |

Arithmetic before a way out: 0 in 61% of ways out on the shell and 72% on Doom, 1 in 23% and 16%. So the hot loop is rarely in a run, and these
snapshots show only what they were doing at that moment. Only lui and arithmetic count as staying in that table; on the card auipc and the others
leave as well (60 and 65 ways out in 100).

**Prices on the card, ns an instruction of the tick's draw, 2 to 5 timed goes of one stretch, repeated:**

| Step | Shape | Doom | Shell |
|---|---|---|---|
| 12 | Arithmetic loop, the rest a way out | 181, 201 | 165, 165 |
| 13 | One loop, a tree, arithmetic first | 188 | 175 |
| 14 | One loop, loads first, then arithmetic | 189 | 177 |

Lean's tick on long runs of the same loads is 175 and 161 (`RESULTS.md`). That is **not like for like**: other code over a longer time, and with what
the new tick still lacks. On these figures the new tick is not ahead on real code. +25% (139) is not reached, and the shape of the loop moves it by 5%
at most, so arithmetic's low price is not what real code sees. A way out's mean (Doom at 188, arithmetic at 67) is about 256 ns, inferred, against 115 to
160 in tight loops; where the rest goes is not known. Run to run Doom moved by about 10%.

**Checks.** Step 12 on both stretches: all 32 registers, the pc and every byte of RAM as the model's (3,015 bytes on Doom, 941 on the shell, none
changed that the model did not write).

**Write cache fill.** `fill`, a store to a new texel each trip: step 8 stopped at 351 of 368 entries (95%), step 10's buckets at 292 (79%). In real code
Doom's stretch had 1 pass in 4 end full, at 7,370 instructions a pass on the mean; the shell's none. Not gpu_vm's 2,000 to 3,000, but one stretch.

**Not settled.** The commit's time reads about 0.5 ns an instruction and is not to be trusted: the timestamps may not separate it from the tick. `--twice`,
to time a pass with the texture's caches warm, read 0.01 to 0.55 ns, which cannot be a pass: the test does not work, and how much of real code's price is
texture waits is still not measured.

## Where it stopped

Next, if the design goes on: why a way out costs 256 on real code against 115 to 160 in tight loops. A SASS listing would say. Floats and the devices for
Quake and nxray; the translated form (not begun). Head-to-head against gpu_vm not run again; no WARP check.
