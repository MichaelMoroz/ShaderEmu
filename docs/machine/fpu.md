# Float instructions

The machine Linux runs on (`nommu.md`) has RISC-V's F extension: single-precision floats in
32 registers of their own. A program built for it does a float addition in one instruction
where the compiler's runtime took about a hundred, which is what made Quake playable
(`docs/games/quake.md`). The smaller machines for bare-metal programs do not have it.

In the shader it is the define `FPU`, which the harness sets for the Linux machine and
`MachineTick.shader` has by hand.

## What is there

| Instructions | |
|---|---|
| `flw`, `fsw` | load and store, the word as it is |
| `fadd.s`, `fsub.s`, `fmul.s`, `fdiv.s`, `fsqrt.s` | |
| `fsgnj.s`, `fsgnjn.s`, `fsgnjx.s` | which are also move, negate and absolute value |
| `fmin.s`, `fmax.s` | a NaN loses to a number, -0 is less than +0 |
| `feq.s`, `flt.s`, `fle.s` | false when either is a NaN |
| `fcvt.w.s`, `fcvt.wu.s` | rounded as the instruction says (a C cast truncates), clamped to the integer's range |
| `fcvt.s.w`, `fcvt.s.wu` | |
| `fmv.x.w`, `fmv.w.x`, `fclass.s` | |

What is not:

- **Fused multiply-add** (`fmadd.s` and its three relatives) is not fused and not fast: it
  reads three registers, the fast step reads two, so it takes the general path and rounds its
  product before adding. Build with `-ffp-contract=off`.
- **Rounding modes.** Arithmetic rounds to nearest whatever the instruction or `frm` says;
  only the conversions to an integer look at the mode. `fcsr`, `frm` and `fflags` can be
  written and read back and nothing acts on them: no exception flag is ever set.
- **`mstatus.FS`.** The instructions work whatever it says, and nothing marks it dirty.
- **Doubles.** There is no D extension (the shader avoids doubles: NVIDIA's D3D12 path
  miscomputes them). A program's doubles stay the runtime's integer routines.

## How exact

`fptest` in the guest runs each instruction on random operands, and on operands chosen to
cancel and to round, and compares with the compiler runtime's integer routines for the same
operation. On the card:

| | Results that differ |
|---|---|
| add, subtract, multiply | none between ordinary numbers |
| compare, every conversion, negate, absolute value | none |
| divide | some by one or two in the last place |
| square root | some by one in the last place |
| anything with a denormal operand or result | the card takes a denormal for zero |

Direct3D asks no more of a card: correctly rounded add, subtract and multiply, divide and
square root within one unit in the last place, and denormals that may be flushed. Results
are not made more exact than the card gives them. What it means in practice: a program's
floats are not bit-for-bit what they are on a real CPU, and may differ between graphics
cards and from `rvc_cpu`'s (`test_card.py`'s `CARD` table has the card's own sums), so a state hash says
nothing about a guest that uses them. Check such a program by what it computes (Quake: its picture).

Three things in the shader are there because the plain expression is wrong, not inexact:

- The card divides by multiplying with a reciprocal. For a large divisor that reciprocal is a
  denormal and the quotient would be zero. Two ordinary numbers are divided
  with their exponents moved towards the middle first.
- Under fxc2, `(float)` of a signed integer can come back as the integer's own bits and `(uint)` of
  a float stops at 2^31. The signed conversion goes by size and sign, the unsigned one in two parts
  from 2^31 up.
- A NaN result is made the one NaN RISC-V specifies, by its bits, whatever the card's looks like.

## In the shader

The float registers are 32 more entries of the array the integer registers are in (`xr`,
f0 at `XR_F`), kept as bits, so `flw`, `fsw` and the moves never go through a float. An
instruction's two source indices get `XR_F` added when they name float registers, and its
result goes to one indexed store with the integers': the fast loop carries nothing more for them.
`fp_exec` in `emu.h` works every result out and chooses one, for the general path and for
the fast step's rarer float instructions; the fast step does a sum, a difference, a product
and `fp_light`'s moves, signs and comparisons each by itself (`tick-costs.md`). Between passes the registers are eight texels of the state zone, from
`FP_STATE_AT`, right after the write cache (`NO_PAGING` has no TLBs' texels before them), and `fcsr` is a
field of core 0's first three texels (`CSR_FIELDS`).

## Programs

Every program in the image is built for it: the toolchain's flags (`linux/userland/toolchain.sh`)
are

    -march=rv32imaf -mabi=ilp32 -ffp-contract=off

and the C library and the compiler's runtime are built with them, so `sin`, `sqrt` and the
rest of the library are float instructions too. A change to those flags builds both again.

- `-mabi=ilp32` keeps floats travelling in integer registers between functions, so what the
  guest's own compiler makes links with the rest.
- `-fno-math-errno` lets `sqrt` be the instruction, and `-fsingle-precision-constant` keeps
  `x * 0.5` from being a double multiplication (a library call and two conversions): a program
  that is floats throughout asks for both, as Quake does.
- `fmadd.s` and its relatives work, on the general path only (a product and a sum, each
  rounded): the C library's `fma` uses them, compiled code is told not to.

**The kernel** keeps each task's float registers (`CONFIG_FPU`, `linux/kernel/fpu_hook.py`).
Linux 5.17 only knows how for a CPU that also has doubles: the hook has it save and restore
single words in the same slots, and take the feature as present whatever the device tree
says. The machine's side is one line: `mstatus.FS` reads dirty, with the summary bit Linux
tests first, whenever it is not off, so the kernel saves at every task switch and no
instruction has to mark anything. Three `fptest` at once must all pass.

An instruction of the machine costs about the same whether it adds integers or floats. Floats pay
where they replace a library call (Quake) and not where a program is integers already (Doom,
Tiberian Dawn) or leaves its geometry with the GPU (glxgears).

## Checking it

    / # fptest            (in the guest; "fptest: done, 0 of 15 instructions wrong")

on `rvc_cpu` and on the card (`test_all.py`'s and `test_card.py`'s `fptest`). A change to the shader's part is
measured whole, as any change to the tick (`AGENTS.md`).
