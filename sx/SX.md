# SX, the machine's own instruction set (draft, 21 October 2026)

A new instruction set for the shader machine, designed for what an iteration of the tick costs, not
after RISC-V. `isa.h` is the encoding. Nothing is built on it yet; this is the design to build.

## Why this shape

Measured on the RISC-V tick (`docs/machine/tick-costs.md`, `experiments/rvc_lean/README.md`): an
operation is about 1.2 ns, a test and its block 16-19 ns, a RAM texel read 36 ns (100 missed), an array
element of a local array 3 to 25 ns; an emulated RISC-V instruction is 115-140 ns on its own path, a
load 450 ns and a store 370 ns with the write cache. Fetch and decode are two fifths of an
instruction's operations. So:

- **One bundle an iteration**: a texel, three op slots and a control word. Fetch, the loop and the pc
  are paid once for up to three ops and a branch.
- **Ops in order** (each sees the one before), so any three consecutive ops share a bundle: packing
  needs no scheduler, only a new bundle at a branch target. The control word acts after the ops.
- **Classes by the top two bits of an op** (integer, memory, float, extended): one or two tests an
  op; inside the integer and float classes every result is made and one chosen.
- **Fewer ops**: a 32-bit immediate wherever an op takes one, `[ra + (rb << s)]` addressing,
  compare-and-branch against a register or an imm6, `sel`, `shadd`, bitfield extract and insert,
  min and max, carries for 64-bit sums, 8 and 16-byte loads and stores (a texel is one write-cache
  op), 64 registers (fewer spills), float and int in one register file.
- **Special functions** on the card's own units: sqrt, rsqrt, rcp, sin, cos, exp2, log2, floor,
  ceil, trunc, round, frac.
- **memcpy, memset and blits by the commit** (an operation region): a store and a `sync`.

Target: two to three times the RISC-V machine's useful work an iteration and a second, measured as
the same C programs (shell loop, factor, gzip, Doom's timedemo, CoreMark) under clang for both
(LLVM is built with RISCV and SX), counts on the reference machines and time on the card.

## Bundle

Four little-endian words at a 16-byte boundary: w0-w2 op slots, w3 the control word. Only the first
`n` op words run (`n` in the control word); an op whose imm12 is -2048 takes the next op word as its
32-bit immediate (the long form; that word counts in `n`). Then the control word.

The pc's low four bits are the word to start at: 0 normally; a trap's EPC is the word of the op that
trapped (4 * slot, 12 for the control word), and `eret` resumes there. A syscall's handler returns
to EPC + 4. Interrupts are taken between bundles only, so a bundle is atomic to its own core's
interrupts (`mfsr`, `and`, `mtsr` in one bundle is an atomic read-modify-write).

### Control word

| bits | |
|---|---|
| 1:0 | n, op words used (0-3) |
| 5:2 | cc |
| cc 0: 8:6 | sub: 0 illegal, 1 none, 2 `jr rs`, 3 `callr rs`, 4 `eret`, 5 `j off23`, 6 `call off23`, 7 illegal |
| cc 0: 14:9 | rs (`jr`, `callr`: pc = rs & ~15) |
| cc 0: 31:9 | off23, bundles from this one (`j`, `call`) |
| cc 1-8 | `beq bne blt bge bltu bgeu` ra, rb; `bbs bbc` ra, bit (rb field) |
| cc 9-14 | `beqi bnei blti bgei bltui bgeui` ra, imm6 (signed; unsigned for the u forms) |
| cc 15 | illegal |
| 11:6, 17:12 | ra, rb or imm6 |
| 31:18 | off14, bundles |

Calls link r1 with the next bundle's address. A zero word is illegal, so a bundle of zeros traps.

### Op word

| bits | |
|---|---|
| 7:0 | opc; 7:6 the class: 0 extended, 1 integer, 2 memory, 3 float |
| 13:8 | rd (writes to r0 are dropped) |
| 19:14 | ra |
| R: 25:20, 31:26 | rb, rc (a register or a 6-bit field, by op) |
| I: 31:20 | imm12, signed; -2048: the next word is the immediate |

**Integer** 0x40 + f (R, B = rb), 0x60 + f (I, B = imm): add, sub (I: rsub = imm - ra), and, or, xor,
andn, orn, xnor, sll, srl, sra, rol, ror, slt, sltu, seq, sne, sgt, sgtu, min, max, minu, maxu, mul;
R only: shadd (ra + (rb << rc)), sel (x[rc] != 0 ? ra : rb), addcy, carry, subby, borrow (carry in x[rc]
& 1); I only: bfe (imm[4:0] pos, imm[9:5] len - 1, imm[10] signed). Shifts take B & 31.

**Memory** 0x80 + mode * 8 + w. Modes: 0 load [ra + imm], 1 store [ra + imm] (value rd), 2 load
[ra + (rb << rc)], 3 store indexed, 4 atomic [ra] with rb (swap add and or xor min max cas), 5 atomic
(minu maxu). Widths: b h w d(rd, rd+1) bu hu q(rd..rd+3). Any alignment (aligned is fast; atomics
must be on a word). cas: rd in the expected word, out the old one; rb the new.

**Float** 0xc0 + f (R), 0xe0 + f (I: B is imm12 << 20 as a float's bits, or the long immediate):
fadd fsub fmul fdiv fmin fmax, fmad fmsb fnmsb (R: with rc, each product and sum rounded), feq flt fle
(an int 0/1), fsgnj fsgnjn fsgnjx, funord, itof utof ftoi ftou (truncate, saturate, NaN 0), ffloor
fceil ftrunc fround (to even), fsqrt frsqrt frcp fsin fcos fexp2 flog2 ffrac. Single precision only
(double in software); denormals flush to zero; sqrt and the special functions are as the card makes
them (approximate: the compiler uses them for sqrtf always and for the rest under -ffast-math).

**Extended** 0x00-0x3f: 0 illegal; mulh mulhu mulhsu div divu rem remu (division by zero: -1 and ra;
INT_MIN / -1: INT_MIN and 0); clz ctz popc bswap brev; bfi (I: inserts ra's low len bits at pos into
rd); lpc (rd = the bundle's address + imm); syscall, break, wait, sync, fence; mfsr, mtsr, xsr, srset,
srclr (I: imm the special register).

`sync` ends the core's pass and waits until the commit has made its operations. `wait` waits for an
interrupt; on a worker it parks. Code stored in a pass runs after a `sync` (the fetch reads RAM as
committed).

## Special registers

0 STATUS (IE, PIE, PS previous mode, S mode read only), 1 EPC, 2 CAUSE, 3 TVAL, 4 TVEC, 5 SCRATCH,
6 IEN, 7 IPEND (bits: 0 software, 1 timer, 2 external), 8-9 TCMP, then readable in user mode:
10-11 TIME (100 kHz), 12 CYCLE (bundles), 13 COREID, 14 PLAYER, 15 RANDOM, 16 LEFT (of the pass),
17 OPS (ops run).

A trap: EPC = where to resume, CAUSE, TVAL; PIE = IE, IE = 0, PS = S, S = 1; pc = TVEC. Causes:
1 illegal (TVAL the word), 2 break, 3 syscall, 4 load fault, 5 store fault (TVAL the address),
6 fetch fault, 7 privilege; interrupts 0x80000000 | the bit. `eret`: pc = EPC, IE = PIE, S = PS.

Two modes, supervisor and user; no machine mode and no firmware: the kernel owns the timer.

## Memory

Every address below the ROM is one texel of the state texture, address >> 4 from row 64: nothing to
work out in the tick.

| | |
|---|---|
| 0x0000_0000 - 0x0000_ffff | read only to the cores (stores fault): the device tree at 0x1000 |
| 0x0001_0000 - 0x01ff_ffff | device memory: the old 0x86000000-0x88000000 window moved down (address - 0x86000000): GPU memory, the mailboxes at 0x00c0_0000, the GPU's control words at 0x0100_0000 |
| 0x0200_0000 - RAM's end | RAM in one piece, to the texture's end (256 MB with 9,280 rows); taller texture, more RAM |
| 0x4000_0000 - 0x5fff_ffff | ROM |
| 0xf000_0000 - | operations: stored to, never RAM; made by the commit |

Stores become visible to the other cores and the devices at the commit, the end of the pass.

**Operations.** A core stores a descriptor texel to 0xf000_0000 (and 0xf000_0010 for a blit) and
then runs `sync`. The descriptor reaches the commit as a write-cache entry like any store; the commit
sees its address is an operation's, makes it from what the core's pass left (its own stores
included), over as many passes as it takes, the core stalled until it is through. Kinds: 1 copy
{dst, src, bytes} (memmove), 2 fill {dst, word, bytes}, 3 blit (+ dst stride, src stride, bytes a
row, rows). One operation in flight a core. The tick has no code for any of it. Other regions can
be mapped to other system operations the same way.

The compiler lowers `memcpy` and `memset` of a known small size to 16-byte loads and stores, and
calls libc for the rest, which uses an operation from a few KB up (a pass ended early costs about
0.2 ms in the harness).

## Workers

As the RISC-V machine's (`docs/machine/multicore.md`): mailboxes at 0x00c0_0000 with the same
layout, a worker starts in user mode at a pc and sp, `wait` parks it, a trap writes a fault record
and parks it, a syscall writes a call record for core 0. Lanes are not part of SX for now.

## ABI (ILP32)

r0 zero, r1 ra, r2 sp, r3 gp (reserved), r4 tp, r5-r12 a0-a7 (r5, r6 the result; r12 the syscall
number), r13-r31 t0-t18, r32-r55 s0-s23 (r32 the frame pointer), r56-r63 t19-t26. Stack 16-byte
aligned. Variadic arguments on the stack; 64-bit values in register pairs; structs up to 8 bytes in
registers, larger by reference; char signed.

ELF: EM_SX 0x5358, relocations in `isa.h`. Programs are bFLT, linked at 0 and not position
independent: every absolute word, a long immediate included, is a bFLT relocation.

## Plan

1. LLVM target SX (llvm-project 23.1.4): TableGen, ISel, frame lowering, MC with bundling in the
   streamer (ops gathered; a label, a control op or three op words close a bundle), asm parser,
   disassembler, ELF writer; lld's SX; clang's target info. Built with X86, RISCV and SX, shared
   libraries (fast relinks).
2. `sx_cpu`: the reference machine (passes, write cache semantics, the commit's operations,
   workers, the new memory map), for the compiler's tests and Linux.
3. compiler-rt builtins, musl's arch/sx, a bFLT converter.
4. Linux 5.17 arch/sx (no MMU): entry, traps, timer, signals, uaccess, the shaderemu drivers.
5. busybox and the benchmarks; boot to a shell on `sx_cpu`.
6. The tick (`sx/tick`): bundles, the classes, a faster write cache; the commit's operations and
   the moved device window; the harness's SX mode. Time against `rvc_lean` on the card. Start from
   `experiments/isa_proto/steps/s21.hlsl` and what `steps/README.md` measured on the way (s18 and s21
   ran the same loops 2.2 to 3.1 times as fast as the lean tick; code and data in one RAM of
   16-byte texels; a write cache of words, committed in four draws; register and cache arrays
   declared 64; a second op fed by selects, not after the write).
7. Workers on SX; then the rest of the programs.
