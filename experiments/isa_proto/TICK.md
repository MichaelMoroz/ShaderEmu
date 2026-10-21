# A shader-native ISA: the prototype tick

`tick.hlsl` is an interpreter for an instruction set laid out for the shader that runs it: as few
instructions as possible, each a block with no switch in it, picked by a 2-bit class and steered by
flag bits. It compiles (fxc2, `ps_5_0`) and has never run: there is no translator, commit pass or
general step yet.

## A slot

The code texture is `R32G32B32A32_UINT`, a slot a texel: `.x` the control word, `.y` ra, `.z` rd
and rb << 16, `.w` the immediate. Every instruction reads `a = r[ra]` and `b = r[rb] + imm`, runs its
class's block, writes `r[rd] = v` and goes to `next`. A float immediate is rb = r0 and imm its bits.

## The control word

Bits 0-1 are the class, tested in this order: INT, MEM, FPU, OTHER (bit 2 set: SFU).

| Class | Bits | Picks or does |
|---|---|---|
| INT | 3-5 | `a + b` (bit 2: `a - b`), and, or, xor, sll, srl, sra, the comparison |
| | 6, 7, 8 | unsigned, equal, inverted: the comparison is a == b, a < b or a <u b, or its inverse |
| | 10 | the comparison picks a or b (min, max, minu, maxu), else gives 1 or 0 |
| | 9, 11-31 | a branch, taken when the comparison holds, to the slot in bits 11-31 |
| MEM | 2, 3 | a store; a load no store of this pass can have touched (the cache is not searched) |
| | 4-8, 9 | 32 less the access's bits (0, 16, 24); a load not sign-extended |
| FPU | 3-5 | `mad`, min, the comparison, sign injection, int to float, float to int, frac, floor |
| | 2, 6, 13 | b, a, the result negated: `max(a, b)` is `-min(-a, -b)` |
| | 7, 8, 9, 10 | `mad(a, m, c)`: m is b or 1, c is b, rd (negated by bit 10) or -0 |
| | 11, 12 | comparison `<=`, `==` (else `<`); sign from `~b`, `a ^ b` (else b) |
| SFU | 3-5 | sin, cos (one `sincos`), exp2, log2, sqrt, rsqrt, `b / a` (rcp: b = 1.0) |
| OTHER | 3, 4, 5-7 | a division, a jump to a + b with rd the link; high word or remainder, signs |
| | 8 | SYS: CSRs, ecall, atomics, fence, traps; the fast loop stops there |

What RISC-V spells as many instructions, the translator spells as one with its flags:

- add, sub, and, or, xor, the shifts, slt, sltu, min, max and all six branches are INT;
- `lui`, `auipc` and `li` are `r0 + imm`, with the constant worked out at translation;
- `jal` is INT with `r0 + imm` (imm the link), compared "not equal" with the link, so always
  taken, to the slot in bits 11-31: no instruction of its own;
- the eight loads and stores are MEM, the float arithmetic `mad`, min, max, `fsgnj`, `fabs`, `fneg`
  and the conversions are FPU, and `fmv` between integer and float registers is INT `r0 + b`.

## Registers

`r[68]`: r0 is zero and never a destination, r1-r31 the integers, r32-r63 the floats as bits, r64
the sink a branch or a stopped instruction writes, 65-67 padding to whole texels.

## Memory

RAM is `R32_UINT`, one word a texel; addresses are bytes, loads and stores naturally aligned. The
write cache is 256 buckets of two (word + 1, value) pairs, picked by the word's low 8 bits, with a
bit a bucket for "written this pass" and the or of stored word numbers as a filter. A store that
finds its bucket full is not made: the pass ends and the next one runs it again.

## Floats

The GPU's: its rounding, its NaNs, `mad` not necessarily fused, no flags and no rounding modes
(float to int truncates).

## Counted

Bytecode instructions in the fast loop with the path pinned (`-D PATH_CLASS=k`, `PATH_STORE`,
`PATH_NO_SEARCH`, `PATH_SFU`), so the compiler leaves only that path; the class tests before it are
one (INT), two (MEM), three (FPU), four (SFU).

| | Instructions |
|---|---|
| INT (every result made, one picked) | 58 |
| MEM, a load the filter lets past | 31 |
| MEM, a load searched in the cache | 53 |
| MEM, a store, every branch in it | 111 |
| FPU | 67 |
| SFU | 27 |
| the shader | 796, 35 temps |

The same loop with a 64-way switch was 18 to 21 instructions for arithmetic and floats, 841 in all.
fxc2 does not tidy selects (it turns a comparison's -1 into 1 and back), and the driver's own
compiler will, so the count overstates the selects more than the switch: the two need timing.
