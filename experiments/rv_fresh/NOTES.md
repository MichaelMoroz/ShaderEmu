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

## What it says so far

- The loop and fetch, the decode, the registers and the choice among most of the ALU come to about 49 ns.
- One piece doubles that: something among sub and the comparisons of L4. All that is measured is the jump from L3 to L4. Which line, and
  why, is not known: a compare may be dear on the card, or the jump may be how the driver compiles the larger loop.
- Control costs as much again (+53), and its code holds more comparisons. That they are the same cause is a guess.
- A core of 8 x 8 costs at most 6% against 4 x 8 for one core, so two warps do not explain gpu_vm's lower price, and core 0 can keep its
  large write cache. This agrees with gpu_vm's own row (every shape within 5% on one core).

## Where it stopped (paused for other work on the card)

`steps\s3b.hlsl` has levels 5 to 9 written and **not measured**, each one piece of L4 on L3: 5 sub by `reg && alt`, 6 sub by `alt`,
7 `a < y`, 8 slt with the sign turned, 9 slt with no compare. Resume:

    pwsh experiments\rv_fresh\batch.ps1 "steps\s3b.hlsl adds -D LEVEL=5 --note L5" ... "steps\s3b.hlsl adds -D LEVEL=9 --note L9"

`adds` has zeros in its registers: use a program with changing data too (`alu` runs only with control, so it needs a level that has it).

Then: control without the dear piece; a load; a store and the write cache, with core 0's 500 texels; a commit. Real code without the whole
machine: start from a snapshot's RAM and registers and run until the first instruction the tick lacks (not built). The translated form
(fixed fields, a texel an instruction) is to be measured against RV's decode as a step of its own, not assumed. The redesign's question,
what a step decodes and how many paths it has, is not answered yet, and no real-use figures exist. gpu_vm's bytecode was read (about 155
instructions a bundle, against about 100 for lean's add, counted by hand), so its lower price is not fewer bytecode instructions per step.

The head-to-head against lean (shell, gzip, Doom, Quake; flat, World, VR) has not been run again.
