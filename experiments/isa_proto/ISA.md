# The machine's instruction set

The machine's own code: a **slot** of 16 bytes, one texel of RAM, holding a main op and an optional second
op. It replaces RV32IMAF as what the cores fetch; the privileged architecture (supervisor mode, its CSRs,
traps, the SBI calls the firmware answers, the worker mailboxes) is the RISC-V machine's as before.
`isa.h` is the encoding, `harness/apps/rvc_cpu.cpp` (`--native`, `native_step`) the reference machine.

## A slot

Four little-endian words, **X Y Z W**, at an address that is a multiple of 16. The pc moves by 16.

| word | bits | field |
|---|---|---|
| X | 0-1 | kind: 0 integer, 1 memory, 2 general (Z's OPG names it); 3 is illegal |
| X | 2 | integer: the sum subtracts; memory: a store |
| X | 3-5 | the integer unit: sum, and, or, xor, sll, srl, sra, cmp |
| X | 6, 7, 8 | cmp: unsigned, equality, inverted |
| X | 9 | a branch on the cmp (or always, bit 29): to pc + W, or (bit 24) to (A + W) & ~15 |
| X | 10 | the branch steps its register first: rd = A + step, compared as stepped |
| X | 11 | the second op is a word load from RF + RG + imm8 |
| X | 12-16 | RG, the second op's second register |
| X | 17-19 | the second unit: sum, and, or, xor, sll, srl, sra, sub |
| X | 20 | the second op stores RG to the word after the main store's |
| X | 21 | the second op is (RF + imm8) & RG |
| X | 22 | W is the second op's immediate; the main op's is 0 |
| X | 23 | link: rd = pc + 16 |
| X | 25-26, 27 | memory width (word, byte, half), zero-extended |
| X | 28 | A is the slot's own address, not a register |
| X | 30-31 | how many RISC-V instructions the slot stands for, 1 to 3; **0 is an illegal slot** |
| Y | 0-4, 5-9, 10-15, 24-31 | RA, RF, RE (6 bits), imm8 (signed) |
| Z | 0-5, 6-10, 12-15, 16-23, 24-28 | RD (6 bits), RB, step (signed), OPG, RC |
| W | 0-31 | the main op's immediate, a branch's offset, a CSR's number |

A written register is 6 bits: 32 is the **sink**, where a result nobody wants goes; x0 always reads 0.
A slot of zeros is illegal, so jumping into cleared memory, or into a zeroed breakpoint, traps.

**Main op.** A = x[RA] (or the pc), B = x[RB] + W (+ 0 for a branch or with bit 22). Integer: RD = A op B.
Memory: the address is A + W; a load writes RD, a store writes x[RD]. Branch: compare A and B, jump.

**Second op.** Made after the main op and seeing its result, unless it is a load (whose address never
depends on it). Unit: RE = x[RF] op2 (x[RG] + imm8, or + W with bit 22). Load: RE = mem[x[RF] + x[RG] + imm8].
Store: mem[main address + 4] = x[RG].

**Precise traps.** Every memory access of a slot is made before any register is written, so a slot that
faults is made again whole after the trap; sepc is the slot's address. A control op is the last op of its
slot and has no second op.

**General ops** (kind 2, OPG): mul, mulh, mulhsu, mulhu, div, divu, rem, remu; lr, sc and the nine AMOs;
the six CSR ops (W the CSR, the ...I forms' RA the immediate); ecall, ebreak, sret, wfi, fence.i, pause,
sfence; flw, fsw and the single-precision float ops of F (W the rounding mode); and **jmap**: a jump to the
RISC-V address in RA through a map at pc + W, (first address, n, n slot offsets), for jumps whose targets
the toolchain could not name; an address outside the map is taken as a slot's.

## Linux

`kernel/patch_native.py` and `kernel/build.sh` make the no-MMU kernel the machine's:

* a system call returns past a slot (sepc + 16; a restart is sepc - 16), a break is a slot long;
* the signal trampoline on the user's stack is two slots (`li a7, __NR_rt_sigreturn; ecall`), 16-aligned;
* the link keeps its relocations (`--emit-relocs --no-relax`) and leaves room for the kernel's slots before
  `_end` (`__native_start`), so memory hands none of it out; the code model is medany;
* binfmt_flat loads a program's slots after its bss (`FLAT_FLAG_NATIVE`: header filler[0] where, filler[1]
  how long; they follow the relocations in the file).

The image starts with a slot that jumps to the kernel's entry; the machine starts there, as before.

## The toolchain

`xlat` makes RISC-V code, as GCC and tcc write it, the machine's: `xlat flat IN.elf OUT.bflt` for a program
(static-PIE, as linux/nommu links them), `xlat kernel vmlinux OUT.bin MAP` for Linux. It translates the
code where it lies: every place that names code (branches, calls, pc-relative addresses, the relocations,
switch tables) is made to name the slot. Names that are bounds of memory, not code (the linker script's
symbols, `_start`), are left as they are. `flat_native.py` makes a whole root, `rom.py` writes its ROM anew,
`image.py` puts kernel and ROM into an image for `rvc_cpu --native`.

Packing: a slot holds 1.43 RISC-V instructions of the kernel and 1.38 of the programs, by fusing pairs
(lui or auipc with addi, auipc with jalr, a step with its branch) and by a second op taken from up to 12
instructions ahead when nothing between depends on it.
