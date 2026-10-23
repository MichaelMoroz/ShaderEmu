// gpu_vm, the machine's own instruction set (gpu_vm/GPU_VM.md): the encoding, shared by the reference machine,
// the tick, the tools and the compiler's tests. C and HLSL both read it (no types but uint).
#ifndef GVM_ISA_H
#define GVM_ISA_H

// ---- A bundle: four words at a 16-byte boundary. Words 0 and 1 are op slots, word 2 a long immediate (for the op
// the control word names), word 3 the control word.
#define GVM_BUNDLE 16u

// Control word (x15's encoding, 22 October 2026; gpu_vm/tick/steps/README.md, "x15"): the path at the bottom (one mask),
// n, whose immediate word 2 is, cc as fields, the compared register, a branch's offset, and the other register or
// imm6 at the top (one shift).
// Its branch compares the registers as they were when this run of the bundle began (before its ops; after a pass
// that ended inside it, where it goes on). A control word of 0 is cc 0, sub 0: illegal (and path 0).
#define GVM_PATH(w) ((w) & 7u)
#define GVM_N(w) (((w) >> 3) & 3u)            // ops, 0-2 (3 is illegal)
#define GVM_LONG0(w) (((w) >> 5) & 1u)       // word 2 is op 0's long immediate
#define GVM_LONG1(w) (((w) >> 6) & 1u)       // op 1's (both bits set: illegal)
#define GVM_CC(w) (((w) >> 7) & 15u)
#define GVM_CRA(w) (((w) >> 11) & 63u)        // a branch's compared register (addbnz: the counted one)
#define GVM_OFF9(w) ((int)((w) << 6) >> 23)   // a branch's offset, in bundles (+-4 KB)
#define GVM_CRB(w) ((w) >> 26)                // the other register, or imm6
#define GVM_CIMM6(w) ((int)(w) >> 26)         // the rb field as a signed imm6 (every immediate form, addbnz)
#define GVM_SUB(w) (((w) >> 11) & 7u)         // cc 0
#define GVM_CRS(w) (((w) >> 14) & 63u)        // cc 0: jr's, callr's and the add's register
#define GVM_OFF18(w) ((int)(w) >> 14)         // cc 0: j's and call's offset, in bundles (+-2 MB)
#define GVM_CIMM12(w) ((int)(w) >> 20)        // cc 0: the add's step
// The paths: which ops a bundle has, so which body of the tick runs it (the assembler's, and trusted). Paths 3-7 are
// the fast loop's (path >= 3): the memory paths 3-6 and the integer path 7 (x17: while an operation is in flight the
// tick's fast loop takes path 7 alone, path >= 7, at no cost). "Mid" is the integer
// functions 0-19, "lite" 0-7 and 16-18 (sums, logic, the three shifts), "float lite" the float functions 0-7.
// Memory ops here are loads and stores of bytes, halves and words. In paths 5 and 7 op 1 reads no register op 0
// writes (the assembler opens a bundle for it), so both ops' addresses and values are known when the bundle starts.
// An empty slot is a word of 0. A bundle whose control word is cc 0 (jr, callr, call, a far j, the add) takes path 1
// when its ops are path 1's or 7's (or it has none), path 2 when they are 2's, 3's or 5's, else 0; eret, and cc 0's
// illegal subs, are path 0 (x18, 22 October 2026: the tick's step12 makes them, not the general step).
#define GVM_PATH_GENERAL 0u                   // anything: the general step
#define GVM_PATH_ALU 1u                       // op 0 an integer op 0-23, sel, lpc (x18) or a float op, op 1 lite or float lite
#define GVM_PATH_MEMF 2u                      // op 0 a memory op, op 1 float lite (or lite in a cc 0 bundle, x18)
#define GVM_PATH_LOAD 3u                      // op 0 a load, op 1 lite
#define GVM_PATH_LOAD2 4u                     // both loads
#define GVM_PATH_STORE 5u                     // op 0 a store, op 1 lite
#define GVM_PATH_MEM2 6u                      // op 0 a store, op 1 a load or a store (a load's store after it: two bundles)
#define GVM_PATH_INT 7u                       // op 0 mid, op 1 lite
#define GVM_PATH_FAST 3u                      // the first of the fast loop's
// cc's bits (7-10): 0 the inverse; 1 less; 2 with 1 signed, without it equal; 3 against imm6 (the rb field, signed:
// the unsigned forms compare with it sign-extended). Neither 1 nor 2: special (cc 0) or addbnz, and with imm6 a bit's
// test (bit imm6 & 31 of ra).
#define GVM_CC_SPECIAL 0u
// ra += imm6 (the rb field, signed), from ra as the bundle found it, after the ops; taken when the sum is not 0. A
// counted loop's decrement and branch in one (bdnz): `addi ra, ra, -1; bnei ra, 0` of the new value. An op of the
// same bundle that writes ra: undefined (the add wins). ra 0 is illegal.
#define GVM_CC_ADDBNZ 1u
#define GVM_CC_BLTU 2u
#define GVM_CC_BGEU 3u
#define GVM_CC_BEQ 4u      // beq r0, r0: a near j (its path the ops')
#define GVM_CC_BNE 5u      // bne r0, r0: falls through (GVM_CTRL_NONE)
#define GVM_CC_BLT 6u
#define GVM_CC_BGE 7u
#define GVM_CC_BBS 8u      // bit imm6[4:0] of ra set
#define GVM_CC_BBC 9u
#define GVM_CC_BLTUI 10u
#define GVM_CC_BGEUI 11u
#define GVM_CC_BEQI 12u
#define GVM_CC_BNEI 13u
#define GVM_CC_BLTI 14u
#define GVM_CC_BGEI 15u
#define GVM_SUB_ILLEGAL 0u
#define GVM_SUB_NONE 1u
#define GVM_SUB_JR 2u
#define GVM_SUB_CALLR 3u
#define GVM_SUB_ERET 4u
#define GVM_SUB_J 5u       // the far j (+-2 MB); a near one is beq r0, r0
#define GVM_SUB_CALL 6u
// The control word's add (fused, 22 October 2026): rs += imm12 (GVM_CRS, GVM_CIMM12) after the bundle's ops, from rs as
// the bundle found it, and the bundle falls through. A pointer's post-increment, a counter, a stack's adjustment in a
// bundle that does not branch, for no op slot. An op of the same bundle that writes rs: undefined (the add wins). rs 0
// is illegal. (cc 0 since x15: the general step's.)
#define GVM_SUB_ADD 7u
// the control word of a bundle that only falls through (bne r0, r0), a near j's (beq r0, r0; the offset at 17), cc 0's
#define GVM_CTRL_NONE (GVM_CC_BNE << 7)
#define GVM_CTRL_J (GVM_CC_BEQ << 7)
#define GVM_CTRL_SPECIAL(sub) ((sub) << 11)

// Op word (x15): ra at the bottom and rb at the top (one op each), rd, opc, then rc (R form) or imm12 over rc and rb
// (I form). The op the control word names takes word 2 as its immediate (the long form), and its own imm12 is
// ignored. An indexed memory op's shift is rc's low 5 bits (w >> 20, as a shift takes them).
#define GVM_RA(w) ((w) & 63u)
#define GVM_RD(w) (((w) >> 6) & 63u)
#define GVM_OPC_SHIFT 12u
#define GVM_OPC(w) (((w) >> 12) & 255u)
#define GVM_RC(w) (((w) >> 20) & 63u)
#define GVM_RB(w) ((w) >> 26)
#define GVM_IMM12(w) ((int)(w) >> 20)
#define GVM_WORD_R(opc, rd, ra, rb, rc) ((opc) << 12 | ((rd) & 63u) << 6 | ((ra) & 63u) | ((rc) & 63u) << 20 | ((rb) & 63u) << 26)
#define GVM_WORD_I(opc, rd, ra, imm12) ((opc) << 12 | ((rd) & 63u) << 6 | ((ra) & 63u) | ((imm12) & 0xfffu) << 20)
#define GVM_CLASS(opc) ((opc) >> 6)            // 0 extended, 1 integer, 2 memory, 3 float
#define GVM_IFORM(opc) (((opc) >> 5) & 1u)     // integer and float classes: the I form

// ---- Integer class: 0x40 + f (R form), 0x60 + f (I form). Numbered for a tree of selects (x15): f[4:3] the
// group; in 0-7, f[1:0] sum, and, or, xor and f[2] the other operand inverted (sub, andn, orn, xnor; the I form's sub
// is ra - imm); in 8-15, f[0] unsigned, f[1] min or max instead of the comparison, f[2] the comparison inverted (sge,
// max); 16-23 the shifts, mul, the rotates and equality. 0-19 are "mid" (the fast loop's op 0), 0-7 and 16-18 "lite".
#define GVM_I_ADD 0u
#define GVM_I_AND 1u
#define GVM_I_OR 2u
#define GVM_I_XOR 3u
#define GVM_I_SUB 4u
#define GVM_I_ANDN 5u
#define GVM_I_ORN 6u
#define GVM_I_XNOR 7u
#define GVM_I_SLT 8u
#define GVM_I_SLTU 9u
#define GVM_I_MIN 10u
#define GVM_I_MINU 11u
#define GVM_I_SGE 12u
#define GVM_I_SGEU 13u
#define GVM_I_MAX 14u
#define GVM_I_MAXU 15u
#define GVM_I_SLL 16u
#define GVM_I_SRL 17u
#define GVM_I_SRA 18u
#define GVM_I_MUL 19u
#define GVM_I_ROR 20u
#define GVM_I_ROL 21u
#define GVM_I_SEQ 22u
#define GVM_I_SNE 23u
#define GVM_I_MID_LAST 19u      // 0-19: the fast loop's op 0
#define GVM_I_COMMON_LAST 23u   // 0-23 in both forms
#define GVM_I_SHADD 24u    // R: ra + (rb << rc), rc the shift itself
#define GVM_I_SEL 25u      // R: x[rc] != 0 ? ra : rb
#define GVM_I_ADDCY 26u    // R: ra + rb + (x[rc] & 1)
#define GVM_I_CARRY 27u    // R: the carry out of that sum
#define GVM_I_SUBBY 28u    // R: ra - rb - (x[rc] & 1)
#define GVM_I_BORROW 29u   // R: the borrow out of that difference
#define GVM_I_BFE 30u      // I: imm[4:0] pos, imm[9:5] len - 1, imm[10] signed
#define GVM_OP_I(f) (0x40u + (f))
#define GVM_OP_II(f) (0x60u + (f))

// ---- Memory class: 0x80 + mode * 8 + w. A byte, a half or a word (b, h, w, bu, hu; modes 0-3) stays in its address's
// word (x17): the bytes past the word are not read or written (a load's are 0, before its extension). d and q, and
// the atomics, are any alignment as before (atomics on a word). The compiler splits a byte, half or word it does not
// know to be in one word.
#define GVM_M_LOAD 0u      // [ra + imm]
#define GVM_M_STORE 1u     // [ra + imm], the value in rd
#define GVM_M_LOADX 2u     // [ra + (rb << rc)]
#define GVM_M_STOREX 3u
#define GVM_M_AMO 4u       // [ra], operand rb
#define GVM_M_AMO2 5u
// The core's file (phase 2, 23 October 2026; gpu_vm/tick/builder/README.md, "The file"): GVM_SR_XR words a core (0: none,
// the ops illegal), private, kept across passes, read and written by index (i = (ra + imm) mod the file's size, in words),
// no tags and no hash. A slot may be bound to a RAM word: bind reads the word into it, sf to a bound slot marks it dirty,
// and the commit writes every dirty bound slot to its word at the pass's end (after the core's write cache, in slot order).
// Pairs: fpair appends (ra + imm, rd) to the core's pair buffer (GVM_SR_NPAIR of them a pass), which the commit writes
// after the bound slots, in order; a full buffer ends the pass before the op. Loads never see a bound slot's or a pair's
// value: a load of a word that is bound, or that has a pair this pass, is the program's error (the reference counts it,
// gvm_core.stale). bind and unbind of a dirty slot end the pass before the op (the commit writes it first).
#define GVM_M_FREAD 6u     // the width field is the kind: GVM_FK_LF, GVM_FK_BIND
#define GVM_M_FWRITE 7u    // GVM_FK_SF, GVM_FK_PAIR, GVM_FK_UNBIND
#define GVM_FK_LF 0u       // rd = F[i]
#define GVM_FK_BIND 1u     // slot i bound to the word at rd's address (word aligned, RAM), F[i] = that word as this core sees it
#define GVM_FK_SF 0u       // F[i] = rd (dirty when bound)
#define GVM_FK_PAIR 1u     // the pair (ra + imm, rd): a word stored at the pass's end
#define GVM_FK_UNBIND 2u   // slot i no longer bound
// widths (x15): bits 1:0 the size (w, d, h, b: a byte or half is extended by a shift of 8 times them), bit 2
// unsigned (w's is q; 5 is illegal)
#define GVM_W_W 0u
#define GVM_W_D 1u         // rd, rd + 1
#define GVM_W_H 2u
#define GVM_W_B 3u
#define GVM_W_Q 4u         // rd .. rd + 3
#define GVM_W_NC 5u        // phase 2: a load mode's word that skips this pass's own stores (read-only data, as __ldg): a load
                           // of a word the core stored this pass is the program's error (the reference counts it as stale)
#define GVM_W_HU 6u
#define GVM_W_BU 7u
#define GVM_AMO_SWAP 0u
#define GVM_AMO_ADD 1u
#define GVM_AMO_AND 2u
#define GVM_AMO_OR 3u
#define GVM_AMO_XOR 4u
#define GVM_AMO_MIN 5u
#define GVM_AMO_MAX 6u
#define GVM_AMO_CAS 7u     // rd in: expected; out: what was there; rb: the new word
#define GVM_AMO2_MINU 0u
#define GVM_AMO2_MAXU 1u
#define GVM_OP_M(mode, w) (0x80u + (mode) * 8u + (w))

// ---- Float class: 0xc0 + f (R form), 0xe0 + f (I form: imm12 is a float's top 12 bits). Numbered for a tree of
// selects: f[4:3] the group; in 0-7 one product and one sum make the first six; 24-31 the card's special functions.
#define GVM_F_ADD 0u
#define GVM_F_MUL 1u
#define GVM_F_MAD 2u       // R: ra * rb + rc, each rounded
#define GVM_F_MSB 3u       // R: ra * rb - rc
#define GVM_F_SUB 4u
#define GVM_F_NMSB 5u      // R: rc - ra * rb
#define GVM_F_MIN 6u
#define GVM_F_MAX 7u
#define GVM_F_EQ 8u
#define GVM_F_LT 9u
#define GVM_F_LE 10u
#define GVM_F_UNORD 11u
#define GVM_F_SGNJ 12u
#define GVM_F_SGNJN 13u
#define GVM_F_SGNJX 14u
#define GVM_F_FRAC 15u
#define GVM_F_ITOF 16u
#define GVM_F_UTOF 17u
#define GVM_F_FTOI 18u
#define GVM_F_FTOU 19u
#define GVM_F_FLOOR 20u
#define GVM_F_CEIL 21u
#define GVM_F_TRUNC 22u
#define GVM_F_ROUND 23u
#define GVM_F_DIV 24u
#define GVM_F_SQRT 25u
#define GVM_F_RSQRT 26u
#define GVM_F_RCP 27u
#define GVM_F_SIN 28u
#define GVM_F_COS 29u
#define GVM_F_EXP2 30u
#define GVM_F_LOG2 31u
#define GVM_OP_F(f) (0xc0u + (f))
#define GVM_OP_FI(f) (0xe0u + (f))

// ---- Extended class: 0x00-0x3f
#define GVM_X_ILL 0x00u
#define GVM_X_MULH 0x01u
#define GVM_X_MULHU 0x02u
#define GVM_X_MULHSU 0x03u
#define GVM_X_DIV 0x04u
#define GVM_X_DIVU 0x05u
#define GVM_X_REM 0x06u
#define GVM_X_REMU 0x07u
#define GVM_X_CLZ 0x08u
#define GVM_X_CTZ 0x09u
#define GVM_X_POPC 0x0au
#define GVM_X_BSWAP 0x0bu
#define GVM_X_BREV 0x0cu
#define GVM_X_BFI 0x0du    // I: imm[4:0] pos, imm[9:5] len - 1; reads rd
#define GVM_X_LPC 0x0eu    // I: rd = the bundle's address + imm
#define GVM_X_SYSCALL 0x10u
#define GVM_X_BREAK 0x11u
#define GVM_X_WAIT 0x12u
#define GVM_X_SYNC 0x13u
#define GVM_X_FENCE 0x14u
#define GVM_X_MFSR 0x18u   // I: rd = sr[imm]
#define GVM_X_MTSR 0x19u   // I: sr[imm] = ra
#define GVM_X_XSR 0x1au    // I: rd = sr[imm], sr[imm] = ra
#define GVM_X_SRSET 0x1bu  // I: rd = sr[imm], sr[imm] |= ra
#define GVM_X_SRCLR 0x1cu  // I: rd = sr[imm], sr[imm] &= ~ra
// ---- Phase 3: 128-bit registers (gvm_machine.vec; the tick's VEC). Every register has four 32-bit lanes; lane 0 is the
// scalar register: a scalar op reads lane 0 and writes lane 0 only (lanes 1-3 keep their values); r0 is 0 in every lane.
// The vector ops (R form, per lane unless noted; floats as the scalar float ops):
#define GVM_X_VADD 0x20u   // ints
#define GVM_X_VSUB 0x21u
#define GVM_X_VMUL 0x22u   // the low 32 bits
#define GVM_X_VAND 0x23u
#define GVM_X_VOR 0x24u
#define GVM_X_VXOR 0x25u
#define GVM_X_VFADD 0x26u
#define GVM_X_VFSUB 0x27u
#define GVM_X_VFMUL 0x28u
#define GVM_X_VFMA 0x29u   // ra * rb + rc
#define GVM_X_VFMIN 0x2au
#define GVM_X_VFMAX 0x2bu
#define GVM_X_VFLT 0x2cu   // ~0 where ra < rb, else 0 (a lane mask)
#define GVM_X_VFLE 0x2du
#define GVM_X_VEQ 0x2eu    // ints: ~0 where equal
#define GVM_X_VSEL 0x2fu   // rc's lane != 0 ? ra : rb (per-lane predication)
#define GVM_X_VDOT 0x30u   // lane 0 the float dot product of ra and rb (a sum of four products, in lane order), lanes 1-3 0
#define GVM_X_VSPLAT 0x31u // every lane ra's lane 0
#define GVM_X_VSWZ 0x32u   // I: lane i is ra's lane (imm >> 2i) & 3
#define GVM_X_VEXT 0x33u   // I: lane 0 (a scalar write) ra's lane imm & 3
#define GVM_X_VINS 0x34u   // I: rd's lane imm & 3 is ra's lane 0, rd's other lanes as they were
#define GVM_X_VLD 0x35u    // I: the 16 bytes at ra + imm (a multiple of 16, else a load fault), lane 0 the lowest
#define GVM_X_VST 0x36u    // I: rd's 16 bytes at ra + imm (a multiple of 16, else a store fault)
#define GVM_X_VMASK 0x37u  // lane 0 (scalar): bit i set when ra's lane i is not 0
#define GVM_X_VFIRST 0x20u
#define GVM_X_VLAST 0x37u

// ---- Special registers
#define GVM_SR_STATUS 0u   // bit 0 IE, 1 PIE, 2 PS (previous mode supervisor), 3 S (read only)
#define GVM_SR_EPC 1u
#define GVM_SR_CAUSE 2u
#define GVM_SR_TVAL 3u
#define GVM_SR_TVEC 4u
#define GVM_SR_SCRATCH 5u
#define GVM_SR_IEN 6u      // bit 0 software, 1 timer, 2 external
#define GVM_SR_IPEND 7u
#define GVM_SR_TCMP_LO 8u
#define GVM_SR_TCMP_HI 9u
#define GVM_SR_TIME_LO 10u // user-readable from here on
#define GVM_SR_TIME_HI 11u
#define GVM_SR_CYCLE 12u
#define GVM_SR_COREID 13u
#define GVM_SR_PLAYER 14u
#define GVM_SR_RANDOM 15u
#define GVM_SR_LEFT 16u
#define GVM_SR_OPS 17u
#define GVM_SR_XR 18u      // the core's file's words (phase 2; read only)
#define GVM_SR_NPAIR 19u   // its pair buffer's pairs a pass (read only)
#define GVM_SR_COUNT 20u
#define GVM_SR_USER_FIRST GVM_SR_TIME_LO
#define GVM_ST_IE 1u
#define GVM_ST_PIE 2u
#define GVM_ST_PS 4u
#define GVM_ST_S 8u

// ---- Causes (CAUSE); an interrupt has bit 31 and the bit of IPEND
#define GVM_CAUSE_ILLEGAL 1u
#define GVM_CAUSE_BREAK 2u
#define GVM_CAUSE_SYSCALL 3u
#define GVM_CAUSE_LOAD 4u
#define GVM_CAUSE_STORE 5u
#define GVM_CAUSE_FETCH 6u
#define GVM_CAUSE_PRIV 7u
#define GVM_CAUSE_IRQ 0x80000000u
#define GVM_IRQ_SOFT 0u
#define GVM_IRQ_TIMER 1u
#define GVM_IRQ_EXT 2u

// ---- The address space
#define GVM_BOOT 0x00000000u      // read-only to the cores: the device tree at GVM_DTB
#define GVM_DTB 0x00001000u
#define GVM_DEV 0x00010000u       // device memory
#define GVM_MBOX 0x00c00000u      // the workers' mailboxes
#define GVM_CONTROL 0x01000000u   // the GPU's control words
#define GVM_RAM 0x02000000u       // RAM, to the state texture's end
#define GVM_ROM 0x40000000u
#define GVM_ROM_END 0x60000000u
// Where texel t (an address >> 4, of RAM or of the ROM's offset) lies in its 8192-wide texture. Map 0 (RAM_MAP in a tick):
// row-major, (t % 8192, t / 8192). Map 1, strips: texel t's 512-texel run is a row of a strip 512 wide and 4096 tall,
// strips side by side (t's bits 0-8 x, 9-20 y, 21 on the strip): a texture row of 8192 falls in few of L1's sets, a
// strip's run of 32 KB does not (tick/probes/codemap, IDEAS 35). A texture under map 1 has 4096 rows, or as many as
// its texels fill when they are fewer than a strip's.
#define GVM_STRIP_X(t) (((t) & 0x1ffu) | (((t) >> 12) & ~0x1ffu))
#define GVM_STRIP_Y(t) (((t) >> 9) & 0xfffu)
#define GVM_STRIP_T(x, y) (((x) & 0x1ffu) | (((y) & 0xfffu) << 9) | (((x) >> 9) << 21))
#define GVM_STRIP_ROWS(texels) ((texels) > 0x200000u ? 4096u : ((texels) + 511u) / 512u)
// Map 2, pairs: texels 2k and 2k + 1 one above the other (the card's L1 sector is 32 bytes, two texels of one column:
// tick/probes/codemap --line), the pairs in rows of 8192: a miss brings the next texel too. A 4 KB block is a box 128 x 2.
#define GVM_PAIR_X(t) (((t) >> 1) & 0x1fffu)
#define GVM_PAIR_Y(t) ((((t) >> 13) & ~1u) | ((t) & 1u))
#define GVM_PAIR_T(x, y) ((((y) >> 1) << 14) | (((x) & 0x1fffu) << 1) | ((y) & 1u))
#define GVM_PAIR_ROWS(texels) ((((texels) + 16383u) / 16384u) * 2u)
#define GVM_OPS 0xf0000000u       // operations: stored to, made by the commit
#define GVM_OP_COPY 1u            // {kind, dst, src, bytes}: memmove
#define GVM_OP_FILL 2u            // {kind, dst, word, bytes}
#define GVM_OP_BLIT 3u            // and at GVM_OPS + 16: {dst stride, src stride, bytes a row, rows}

// ---- ELF
#define EM_GVM 0x4756
#define R_GVM_NONE 0
#define R_GVM_32 1
#define R_GVM_REL32 2
#define R_GVM_IMM32 3      // an op's long immediate: S + A
#define R_GVM_PCIMM32 4    // lpc's long immediate: S + A - the bundle
#define R_GVM_BR9 5        // control word [25:17]: (S + A - the bundle) / 16
#define R_GVM_J18 6        // control word [31:14] (cc 0: the far j, call)
#define R_GVM_IMM12 7      // op word [31:20]: S + A, which must fit
#define R_GVM_TPREL32 8    // a long immediate: S + A, from the thread pointer
#define R_GVM_RELATIVE 9

#endif
