# Float instructions

The full machine (the one Linux runs on) has RISC-V's F extension: single-precision floats in
32 registers of their own. A program built for it does a float addition in one instruction
where the compiler's runtime took about a hundred, which is what made Quake playable
(`docs/quake.md`). The smaller machines for bare-metal programs do not have it.

In the shader it is the define `FPU`, which the harness sets for the full machine and
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
operation. Of 20,000 pairs, the same on D3D11 with fxc2 and on D3D12 with DXC (an RTX 5090):

| | Results that differ |
|---|---|
| add, subtract, multiply | none between ordinary numbers |
| compare, every conversion, negate, absolute value | none |
| divide | 3,986 by one in the last place, 10 by two |
| square root | 1,229 by one in the last place |
| anything with a denormal operand or result | the card takes a denormal for zero (106 of the additions, 462 of the multiplications) |

Direct3D asks no more of a card: correctly rounded add, subtract and multiply, divide and
square root within one unit in the last place, and denormals that may be flushed. Results
are not made more exact than the card gives them. What it means in practice: a program's
floats are not bit-for-bit what they are on a real CPU, and may differ between graphics
cards, so a state hash says nothing about a guest that uses them. Check such a program by
what it computes (Quake: its picture, the same on both backends).

Three things in the shader are there because the plain expression was wrong, not inexact:

- The card divides by multiplying with a reciprocal. For a large divisor that reciprocal is a
  denormal, so a quotient of two large numbers came out zero. Two ordinary numbers are divided
  with their exponents moved towards the middle first.
- Under fxc2, `(float)` of a signed integer came back as the integer's own bits (19,977 of
  20,000 wrong) and `(uint)` of a float stopped at 2^31. The signed conversion goes by size
  and sign, the unsigned one in two parts from 2^31 up.
- A NaN result is made the one NaN RISC-V specifies, by its bits, whatever the card's looks like.

## In the shader

The float registers are 32 more entries of the array the integer registers are in (`xr`,
f0 at `XR_F`), kept as bits, so `flw`, `fsw` and the moves never go through a float. An
instruction's two source indices get `XR_F` added when they name float registers, and its
result goes to one indexed store as before: the fast loop carries nothing more than it did.
`fp_exec` in `emu.h` works every result out and chooses one, for the fast step and the
general path alike. Between passes the registers are eight texels of the state zone, from
`FP_STATE_AT`, after the TLBs' texels.

An integer-only guest costs 1.1% more with them (D3D11, fxc2: 3,556k to 3,518k instructions a
second on the shell busy loop, four runs each, alternating) and reaches the same state hash.

## Programs

Every program in the image is built for it: the toolchain's flags (`linux/userland/toolchain.sh`)
are

    -march=rv32imaf -mabi=ilp32 -ffp-contract=off

and the C library and the compiler's runtime are built with them, so `sin`, `sqrt` and the
rest of the library are float instructions too. A change to those flags builds both again.

- `-mabi=ilp32` keeps floats travelling in integer registers between functions, so objects
  built before and after link together, and so does what the guest's own compiler makes.
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
instruction has to mark anything. Three `fptest` at once all pass; before, each had 5 or 6 of
15 instructions wrong.

What it was worth, in instructions (the image before and after):

| | before | after |
|---|---|---|
| Quake, a frame at the start of e1m1 | 1,810k | 752k to 795k |
| glxgears, a frame | about 8k | 6.4k |
| Doom, demo1: a tic, a frame | 26k to 43k, 33k to 50k | 34k to 41k, 34k to 66k (other moments of the demo: no change to speak of) |

An instruction of the machine costs the same whether it adds integers or floats. Floats pay
where they replace a library call (Quake) and not where a program was integers already (Doom,
Tiberian Dawn) or leaves its geometry with the GPU (glxgears).

## Checking it

    / # fptest            (in the guest; "fptest: done, 0 of 15 instructions wrong")

and, after a change to the shader's part, the integer benchmark with and without `FPU` for
its cost and its state hash.
