# gpu_vm assembly (LLVM's `gpuvm` target)

What `llvm-mc -triple=gpuvm` and clang's integrated assembler take, and what `llvm-objdump -d` prints. The encoding is
`gpu_vm/isa.h`; what each op does is `gpu_vm/ref/gvm_ref.h`. The packing of ops into bundles copies `gpu_vm/ref/gvm_asm.h`.

## Lines

- One statement a line, or several separated by `;`. Comments: `#` or `//` to the end of the line, `/* */`.
- `;;` at the end of a statement (`add r5, r6, r7 ;;`) closes the open bundle after it; so does `.endbundle` on a line of
  its own. Neither is ever needed: the assembler closes bundles by itself (below).
- Labels `name:`, numeric local labels `1:` / `1b` / `1f`, `.L` local symbols. Integers decimal, `0x`, `0b`, `0`
  octal, and `'c'`.

## Registers

`r0`-`r63`, or the ABI's names (ILP32, `GPU_VM.md` "ABI"):

| | |
|---|---|
| `zero` | r0 (reads 0, writes are dropped) |
| `ra` | r1 (the link: `call`, `callr`) |
| `sp` | r2 |
| `gp` | r3 (reserved) |
| `tp` | r4 |
| `a0`-`a7` | r5-r12 (a0, a1 the result; a7 = r12 the syscall number) |
| `t0`-`t18` | r13-r31 |
| `s0`-`s23` | r32-r55; `fp` = `s0` = r32 |
| `t19`-`t26` | r56-r63 |

A float is a word in the same registers. The disassembler prints the ABI names.

## Ops

An op is one op word (and a long immediate, see below). `rd` is written, `ra`, `rb`, `rc` read.

### Integer (0x40 + f R form, 0x60 + f I form)

R form `op rd, ra, rb`; I form `opi rd, ra, imm` (imm any 32-bit value or a symbol's expression):

| R | I | |
|---|---|---|
| `add` | `addi` | |
| `sub` | `subi` | ra - rb, ra - imm |
| `and` `or` `xor` | `andi` `ori` `xori` | |
| `andn` `orn` `xnor` | `andni` `orni` `xnori` | ra op ~B |
| `slt` `sltu` | `slti` `sltui` | |
| `sge` `sgeu` | `sgei` `sgeui` | |
| `min` `minu` `max` `maxu` | `mini` `minui` `maxi` `maxui` | |
| `sll` `srl` `sra` `rol` `ror` | `slli` `srli` `srai` `roli` `rori` | B & 31 |
| `mul` | `muli` | low word |
| `seq` `sne` | `seqi` `snei` | |

R form only, four operands:

| | |
|---|---|
| `shadd rd, ra, rb, sh` | ra + (rb << sh), sh 0-31 |
| `sel rd, ra, rb, rc` | rc != 0 ? ra : rb |
| `addcy rd, ra, rb, rc` / `carry rd, ra, rb, rc` | ra + rb + (rc & 1), its carry out |
| `subby rd, ra, rb, rc` / `borrow rd, ra, rb, rc` | ra - rb - (rc & 1), its borrow out |

I form only: `bfe rd, ra, pos, len` (zero-extended) and `bfes rd, ra, pos, len` (sign-extended): the field of `len`
(1-32) bits at bit `pos` (0-31).

### Memory (0x80 + mode * 8 + w)

Widths: `b` `h` `w` signed byte and half, word; `bu` `hu`; `d` two words (rd, rd + 1); `q` four (rd .. rd + 3).
Any alignment.

| | |
|---|---|
| `lb lh lw ld lq lbu lhu rd, imm(ra)` | load [ra + imm]; `(ra)` is `0(ra)`, `sym(ra)` and `sym` (= `sym(zero)`) a long immediate |
| `sb sh sw sd sq rs, imm(ra)` | store rs (rs .. rs + 3) |
| `lbx lhx lwx ldx lqx lbux lhux rd, ra, rb, sh` | load [ra + (rb << sh)], sh 0-31 |
| `sbx shx swx sdx sqx rs, ra, rb, sh` | store indexed |
| `amoswap amoadd amoand amoor amoxor amomin amomax amominu amomaxu rd, rb, (ra)` | rd = [ra], [ra] = [ra] op rb (a word) |
| `amocas rd, rb, (ra)` | rd in: the expected word, out: what was there; rb the new word |

### Float (0xc0 + f R form, 0xe0 + f I form), single precision

| | |
|---|---|
| `fadd fsub fmul fdiv fmin fmax rd, ra, rb` | |
| `feq flt fle funord rd, ra, rb` | 0 or 1 |
| `fsgnj fsgnjn fsgnjx rd, ra, rb` | |
| `fmad fmsb fnmsb rd, ra, rb, rc` | ra * rb + rc, ra * rb - rc, rc - ra * rb (each rounded) |
| `ffrac ffloor fceil ftrunc fround fsqrt frsqrt frcp fsin fcos fexp2 flog2 rd, ra` | |
| `itof utof ftoi ftou rd, ra` | ftoi, ftou truncate and saturate (NaN 0) |
| `faddi fsubi fmuli fdivi fmini fmaxi feqi flti flei funordi fsgnji fsgnjni fsgnjxi rd, ra, fimm` | B an immediate float |

`fimm` is a float literal (`1.5`, `-2.0e-3`, `inf`, `nan`) or an integer, taken as the float's bits (`0x3fc00000`):
`1` is the bits 1 (a denormal), write `1.0`. It is short (imm12, the top 12 bits) when its low 20 bits are 0, else long.

### Extended (0x00-0x3f)

| | |
|---|---|
| `mulh mulhu mulhsu div divu rem remu rd, ra, rb` | |
| `clz ctz popc bswap brev rd, ra` | |
| `bfi rd, ra, pos, len` | inserts ra's low `len` bits at `pos` into rd (reads rd) |
| `lpc rd, sym` | rd = sym's address, PC-relative (a long immediate; a number is a byte offset from the bundle) |
| `syscall` `break` `wait` `sync` `fence` | |
| `unimp` | the op word 0 (illegal) |
| `mfsr rd, sr` | rd = sr |
| `mtsr sr, ra` | sr = ra |
| `xsr rd, sr, ra` `srset rd, sr, ra` `srclr rd, sr, ra` | rd = sr, then sr = ra, sr \|= ra, sr &= ~ra |

`sr` is a number (0-4095) or a name: `status epc cause tval tvec scratch ien ipend tcmp_lo tcmp_hi time_lo time_hi
cycle coreid player random left ops` (0-17, `isa.h`).

## Immediates: short and long

An I form op has a 12-bit signed immediate in its op word (-2048..2047; a float's top 12 bits), or takes the bundle's
word 2 as a whole 32-bit immediate (the long form, one a bundle). The assembler chooses: a constant that fits is
short, anything else long, and any symbol's expression is long (with a relocation). So `addi a0, zero, sym`,
`lw a0, sym` and `sw a0, sym+8(t0)` are one op each. There is no `%hi`/`%lo`.

Operators, in a long immediate only: `%tprel(sym)` (TLS, from the thread pointer: R_GVM_TPREL32).

## Control ops

At most one a bundle, in its control word, after its ops. A control op closes the bundle.

| | |
|---|---|
| `beq bne blt bge bltu bgeu ra, rb, label` | |
| `beqi bnei blti bgei bltui bgeui ra, imm6, label` | imm6 -32..31 (the unsigned forms compare with it sign-extended) |
| `bbs bbc ra, bit, label` | bit 0-31 of ra set, clear |
| `addbnz ra, imm6, label` | ra += imm6, and to label when the sum is not 0 (ra not r0) |
| `j label` | near (+-4 KB: `beq r0, r0`) or far (+-2 MB): chosen when the label is known; to another section or symbol, far |
| `call label` | r1 = the next bundle, +-2 MB |
| `jr rs` `callr rs` | to rs & ~15 (callr links r1); rs as the bundle's ops left it |
| `ret` | `jr ra` |
| `eret` | |
| `bump rs, imm12` | rs += imm12 after the bundle's ops (no branch; rs not r0) |

Branches reach +-256 bundles (+-4 KB): farther is an error (branch relaxation is the compiler's). A label is
`sym`, `sym+off`, `1f`; a number is a byte offset from the branch's bundle (a multiple of 16), which is what the
disassembler prints as an address.

Every control op has program order: a branch compares registers as the ops before it left them, `addbnz` and
`bump` add to the register as the ops before them left it. (The hardware compares and adds from the registers as the
bundle found them: the assembler opens a new bundle where that would differ, see below.)

## Pseudo-instructions

| | |
|---|---|
| `nop` | `add zero, zero, zero` |
| `li rd, imm` | `addi rd, zero, imm` (one op, short or long) |
| `la rd, sym` | `addi rd, zero, sym` (absolute: R_GVM_IMM32) |
| `lla rd, sym` | `lpc rd, sym` (PC-relative) |
| `mv rd, ra` | `addi rd, ra, 0` |
| `not rd, ra` | `xori rd, ra, -1` |
| `neg rd, ra` | `sub rd, zero, ra` |
| `seqz rd, ra` / `snez rd, ra` | `seqi rd, ra, 0` / `snei rd, ra, 0` |
| `sgt sgtu sle sleu rd, ra, rb` | `slt sltu sge sgeu rd, rb, ra` |
| `fmv rd, ra` / `fneg` / `fabs` | `addi rd, ra, 0` / `fsgnjn rd, ra, ra` / `fsgnjx rd, ra, ra` |
| `fgt fge rd, ra, rb` | `flt fle rd, rb, ra` |
| `beqz bnez bltz bgez rd, label` | `beq bne blt bge ra, zero, label` |
| `blez bgtz ra, label` | `bge blt zero, ra, label` |
| `bgt ble bgtu bleu ra, rb, label` | `blt bge bltu bgeu rb, ra, label` |
| `tail sym` | `j sym` |

## Bundles

Every bundle is 16 bytes at a 16-byte boundary: op 0, op 1, the long immediate, the control word. Ops go into the
open bundle in order; the assembler closes it (gvm_asm.h's rules) when:

- it holds two ops, or the next op does not fit as op 1: op 1 must be a "lite" integer op (functions 0-7 and the
  shifts, R or I), or a "float lite" one (float functions 0-7) after a float or memory op 0, or a memory op after a
  memory op 0 that reads no register op 0 writes (a store's value included) and is not a store after a load;
- the next op wants the long immediate and the bundle's is taken;
- a control op is put (it closes the bundle after itself); a branch that compares a register an op of the bundle
  writes, or an `addbnz` whose register one writes, goes into a new bundle of its own;
- a `bump` is pending and the next op names its register, or the next is a control op;
- a label, a section change, an alignment, data, `.endbundle` or `;;`, the end of the file.

A bundle with no control op falls through (`bne r0, r0`). The control word's path (bits 2:0) is gvm_asm.h's
`path()` of the bundle's ops (0 for cc 0: `jr`, `callr`, `call`, `eret`, a far `j`, `bump`). Code alignment fills
with empty bundles. `llc -mllvm -gpuvm-no-pack` (or `llvm-mc -gpuvm-no-pack`) puts one op in a bundle (a control op
joins the op before it as above), for debugging.

`.` in an op's operand is the start of the bundle the op goes into (a label: it closes the open one).

## Directives

The GNU ones: `.text .data .rodata .bss .section .pushsection .popsection .globl .local .weak .hidden .type .size
.set .equ .comm .lcomm .byte .half .2byte .word .4byte .dword .8byte .ascii .asciz .string .zero .space .fill
.align .p2align .balign .org .ident .file .loc .cfi_*`, macros and conditionals. `.align n` is 2^n bytes (as
`.p2align`). `.word sym` is R_GVM_32, `.word sym - .` R_GVM_REL32. `.endbundle` closes the open bundle.

## Relocations (isa.h)

| | field | value |
|---|---|---|
| R_GVM_32 | a data word | S + A |
| R_GVM_REL32 | a data word | S + A - P |
| R_GVM_IMM32 | word 2 (the long immediate) | S + A |
| R_GVM_PCIMM32 | word 2 of `lpc` | S + A - the bundle |
| R_GVM_BR9 | control word [25:17] | (S + A - the bundle) / 16 |
| R_GVM_J18 | control word [31:14] | (S + A - the bundle) / 16, cc 0 (far `j`, `call`) |
| R_GVM_IMM12 | op word [31:20] | S + A, must fit (not made by the assembler yet) |
| R_GVM_TPREL32 | word 2 | S + A - TLS's start (`%tprel`) |

## Disassembly

`llvm-objdump -d` prints a bundle on one line: its ops, then its control op, separated by `;`, and `;;` at the end
(an empty bundle is `;;` alone); a branch's target as an address. A bundle whose path bits are not what the
assembler would make gets `# path N` after it.
