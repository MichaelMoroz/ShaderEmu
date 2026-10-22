// SX, the machine's own instruction set (sx/SX.md): the encoding, shared by the reference machine,
// the tick, the tools and the compiler's tests. C and HLSL both read it (no types but uint).
#ifndef SX_ISA_H
#define SX_ISA_H

// ---- A bundle: four words at a 16-byte boundary. Words 0 and 1 are op slots, word 2 a long immediate (for the op
// the control word names), word 3 the control word.
#define SX_BUNDLE 16u

// Control word (x15's encoding, 22 October 2026; sx/tick/steps/README.md, "x15"): the path at the bottom (one mask),
// n, whose immediate word 2 is, cc as fields, the compared register, a branch's offset, and the other register or
// imm6 at the top (one shift).
// Its branch compares the registers as they were when this run of the bundle began (before its ops; after a pass
// that ended inside it, where it goes on). A control word of 0 is cc 0, sub 0: illegal (and path 0).
#define SX_PATH(w) ((w) & 7u)
#define SX_N(w) (((w) >> 3) & 3u)            // ops, 0-2 (3 is illegal)
#define SX_LONG0(w) (((w) >> 5) & 1u)       // word 2 is op 0's long immediate
#define SX_LONG1(w) (((w) >> 6) & 1u)       // op 1's (both bits set: illegal)
#define SX_CC(w) (((w) >> 7) & 15u)
#define SX_CRA(w) (((w) >> 11) & 63u)        // a branch's compared register (addbnz: the counted one)
#define SX_OFF9(w) ((int)((w) << 6) >> 23)   // a branch's offset, in bundles (+-4 KB)
#define SX_CRB(w) ((w) >> 26)                // the other register, or imm6
#define SX_CIMM6(w) ((int)(w) >> 26)         // the rb field as a signed imm6 (every immediate form, addbnz)
#define SX_SUB(w) (((w) >> 11) & 7u)         // cc 0
#define SX_CRS(w) (((w) >> 14) & 63u)        // cc 0: jr's, callr's and the add's register
#define SX_OFF18(w) ((int)(w) >> 14)         // cc 0: j's and call's offset, in bundles (+-2 MB)
#define SX_CIMM12(w) ((int)(w) >> 20)        // cc 0: the add's step
// The paths: which ops a bundle has, so which body of the tick runs it (the assembler's, and trusted). Paths 3-7 are
// the fast loop's (path >= 3); in 4-7 bit 0 says two memory ops and bit 1 a store among them. "Mid" is the integer
// functions 0-19, "lite" 0-7 and 16-18 (sums, logic, the three shifts), "float lite" the float functions 0-7.
// Memory ops here are loads and stores of bytes, halves and words. In paths 5 and 7 op 1 reads no register op 0
// writes (the assembler opens a bundle for it), so both ops' addresses and values are known when the bundle starts.
// An empty slot is a word of 0. A bundle whose control word is cc 0 (jr, callr, call, eret, a far j, the add) is the
// general step's (path 0).
#define SX_PATH_GENERAL 0u                   // anything: the general step
#define SX_PATH_ALU 1u                       // op 0 an integer op 0-23 or a float op, op 1 lite or float lite
#define SX_PATH_MEMF 2u                      // op 0 a memory op, op 1 float lite
#define SX_PATH_INT 3u                       // op 0 mid, op 1 lite
#define SX_PATH_LOAD 4u                      // op 0 a load, op 1 lite
#define SX_PATH_LOAD2 5u                     // both loads
#define SX_PATH_STORE 6u                     // op 0 a store, op 1 lite
#define SX_PATH_MEM2 7u                      // op 0 a store, op 1 a load or a store (a load's store after it: two bundles)
#define SX_PATH_FAST 3u                      // the first of the fast loop's
// cc's bits (7-10): 0 the inverse; 1 less; 2 with 1 signed, without it equal; 3 against imm6 (the rb field, signed:
// the unsigned forms compare with it sign-extended). Neither 1 nor 2: special (cc 0) or addbnz, and with imm6 a bit's
// test (bit imm6 & 31 of ra).
#define SX_CC_SPECIAL 0u
// ra += imm6 (the rb field, signed), from ra as the bundle found it, after the ops; taken when the sum is not 0. A
// counted loop's decrement and branch in one (bdnz): `addi ra, ra, -1; bnei ra, 0` of the new value. An op of the
// same bundle that writes ra: undefined (the add wins). ra 0 is illegal.
#define SX_CC_ADDBNZ 1u
#define SX_CC_BLTU 2u
#define SX_CC_BGEU 3u
#define SX_CC_BEQ 4u      // beq r0, r0: a near j (its path the ops')
#define SX_CC_BNE 5u      // bne r0, r0: falls through (SX_CTRL_NONE)
#define SX_CC_BLT 6u
#define SX_CC_BGE 7u
#define SX_CC_BBS 8u      // bit imm6[4:0] of ra set
#define SX_CC_BBC 9u
#define SX_CC_BLTUI 10u
#define SX_CC_BGEUI 11u
#define SX_CC_BEQI 12u
#define SX_CC_BNEI 13u
#define SX_CC_BLTI 14u
#define SX_CC_BGEI 15u
#define SX_SUB_ILLEGAL 0u
#define SX_SUB_NONE 1u
#define SX_SUB_JR 2u
#define SX_SUB_CALLR 3u
#define SX_SUB_ERET 4u
#define SX_SUB_J 5u       // the far j (+-2 MB); a near one is beq r0, r0
#define SX_SUB_CALL 6u
// The control word's add (fused, 22 October 2026): rs += imm12 (SX_CRS, SX_CIMM12) after the bundle's ops, from rs as
// the bundle found it, and the bundle falls through. A pointer's post-increment, a counter, a stack's adjustment in a
// bundle that does not branch, for no op slot. An op of the same bundle that writes rs: undefined (the add wins). rs 0
// is illegal. (cc 0 since x15: the general step's.)
#define SX_SUB_ADD 7u
// the control word of a bundle that only falls through (bne r0, r0), a near j's (beq r0, r0; the offset at 17), cc 0's
#define SX_CTRL_NONE (SX_CC_BNE << 7)
#define SX_CTRL_J (SX_CC_BEQ << 7)
#define SX_CTRL_SPECIAL(sub) ((sub) << 11)

// Op word (x15): ra at the bottom and rb at the top (one op each), rd, opc, then rc (R form) or imm12 over rc and rb
// (I form). The op the control word names takes word 2 as its immediate (the long form), and its own imm12 is
// ignored. An indexed memory op's shift is rc's low 5 bits (w >> 20, as a shift takes them).
#define SX_RA(w) ((w) & 63u)
#define SX_RD(w) (((w) >> 6) & 63u)
#define SX_OPC_SHIFT 12u
#define SX_OPC(w) (((w) >> 12) & 255u)
#define SX_RC(w) (((w) >> 20) & 63u)
#define SX_RB(w) ((w) >> 26)
#define SX_IMM12(w) ((int)(w) >> 20)
#define SX_WORD_R(opc, rd, ra, rb, rc) ((opc) << 12 | ((rd) & 63u) << 6 | ((ra) & 63u) | ((rc) & 63u) << 20 | ((rb) & 63u) << 26)
#define SX_WORD_I(opc, rd, ra, imm12) ((opc) << 12 | ((rd) & 63u) << 6 | ((ra) & 63u) | ((imm12) & 0xfffu) << 20)
#define SX_CLASS(opc) ((opc) >> 6)            // 0 extended, 1 integer, 2 memory, 3 float
#define SX_IFORM(opc) (((opc) >> 5) & 1u)     // integer and float classes: the I form

// ---- Integer class: 0x40 + f (R form), 0x60 + f (I form). Numbered for a tree of selects (x15): f[4:3] the
// group; in 0-7, f[1:0] sum, and, or, xor and f[2] the other operand inverted (sub, andn, orn, xnor; the I form's sub
// is ra - imm); in 8-15, f[0] unsigned, f[1] min or max instead of the comparison, f[2] the comparison inverted (sge,
// max); 16-23 the shifts, mul, the rotates and equality. 0-19 are "mid" (the fast loop's op 0), 0-7 and 16-18 "lite".
#define SX_I_ADD 0u
#define SX_I_AND 1u
#define SX_I_OR 2u
#define SX_I_XOR 3u
#define SX_I_SUB 4u
#define SX_I_ANDN 5u
#define SX_I_ORN 6u
#define SX_I_XNOR 7u
#define SX_I_SLT 8u
#define SX_I_SLTU 9u
#define SX_I_MIN 10u
#define SX_I_MINU 11u
#define SX_I_SGE 12u
#define SX_I_SGEU 13u
#define SX_I_MAX 14u
#define SX_I_MAXU 15u
#define SX_I_SLL 16u
#define SX_I_SRL 17u
#define SX_I_SRA 18u
#define SX_I_MUL 19u
#define SX_I_ROR 20u
#define SX_I_ROL 21u
#define SX_I_SEQ 22u
#define SX_I_SNE 23u
#define SX_I_MID_LAST 19u      // 0-19: the fast loop's op 0
#define SX_I_COMMON_LAST 23u   // 0-23 in both forms
#define SX_I_SHADD 24u    // R: ra + (rb << rc), rc the shift itself
#define SX_I_SEL 25u      // R: x[rc] != 0 ? ra : rb
#define SX_I_ADDCY 26u    // R: ra + rb + (x[rc] & 1)
#define SX_I_CARRY 27u    // R: the carry out of that sum
#define SX_I_SUBBY 28u    // R: ra - rb - (x[rc] & 1)
#define SX_I_BORROW 29u   // R: the borrow out of that difference
#define SX_I_BFE 30u      // I: imm[4:0] pos, imm[9:5] len - 1, imm[10] signed
#define SX_OP_I(f) (0x40u + (f))
#define SX_OP_II(f) (0x60u + (f))

// ---- Memory class: 0x80 + mode * 8 + w
#define SX_M_LOAD 0u      // [ra + imm]
#define SX_M_STORE 1u     // [ra + imm], the value in rd
#define SX_M_LOADX 2u     // [ra + (rb << rc)]
#define SX_M_STOREX 3u
#define SX_M_AMO 4u       // [ra], operand rb
#define SX_M_AMO2 5u
// widths (x15): bits 1:0 the size (w, d, h, b: a byte or half is extended by a shift of 8 times them), bit 2
// unsigned (w's is q; 5 is illegal)
#define SX_W_W 0u
#define SX_W_D 1u         // rd, rd + 1
#define SX_W_H 2u
#define SX_W_B 3u
#define SX_W_Q 4u         // rd .. rd + 3
#define SX_W_HU 6u
#define SX_W_BU 7u
#define SX_AMO_SWAP 0u
#define SX_AMO_ADD 1u
#define SX_AMO_AND 2u
#define SX_AMO_OR 3u
#define SX_AMO_XOR 4u
#define SX_AMO_MIN 5u
#define SX_AMO_MAX 6u
#define SX_AMO_CAS 7u     // rd in: expected; out: what was there; rb: the new word
#define SX_AMO2_MINU 0u
#define SX_AMO2_MAXU 1u
#define SX_OP_M(mode, w) (0x80u + (mode) * 8u + (w))

// ---- Float class: 0xc0 + f (R form), 0xe0 + f (I form: imm12 is a float's top 12 bits). Numbered for a tree of
// selects: f[4:3] the group; in 0-7 one product and one sum make the first six; 24-31 the card's special functions.
#define SX_F_ADD 0u
#define SX_F_MUL 1u
#define SX_F_MAD 2u       // R: ra * rb + rc, each rounded
#define SX_F_MSB 3u       // R: ra * rb - rc
#define SX_F_SUB 4u
#define SX_F_NMSB 5u      // R: rc - ra * rb
#define SX_F_MIN 6u
#define SX_F_MAX 7u
#define SX_F_EQ 8u
#define SX_F_LT 9u
#define SX_F_LE 10u
#define SX_F_UNORD 11u
#define SX_F_SGNJ 12u
#define SX_F_SGNJN 13u
#define SX_F_SGNJX 14u
#define SX_F_FRAC 15u
#define SX_F_ITOF 16u
#define SX_F_UTOF 17u
#define SX_F_FTOI 18u
#define SX_F_FTOU 19u
#define SX_F_FLOOR 20u
#define SX_F_CEIL 21u
#define SX_F_TRUNC 22u
#define SX_F_ROUND 23u
#define SX_F_DIV 24u
#define SX_F_SQRT 25u
#define SX_F_RSQRT 26u
#define SX_F_RCP 27u
#define SX_F_SIN 28u
#define SX_F_COS 29u
#define SX_F_EXP2 30u
#define SX_F_LOG2 31u
#define SX_OP_F(f) (0xc0u + (f))
#define SX_OP_FI(f) (0xe0u + (f))

// ---- Extended class: 0x00-0x3f
#define SX_X_ILL 0x00u
#define SX_X_MULH 0x01u
#define SX_X_MULHU 0x02u
#define SX_X_MULHSU 0x03u
#define SX_X_DIV 0x04u
#define SX_X_DIVU 0x05u
#define SX_X_REM 0x06u
#define SX_X_REMU 0x07u
#define SX_X_CLZ 0x08u
#define SX_X_CTZ 0x09u
#define SX_X_POPC 0x0au
#define SX_X_BSWAP 0x0bu
#define SX_X_BREV 0x0cu
#define SX_X_BFI 0x0du    // I: imm[4:0] pos, imm[9:5] len - 1; reads rd
#define SX_X_LPC 0x0eu    // I: rd = the bundle's address + imm
#define SX_X_SYSCALL 0x10u
#define SX_X_BREAK 0x11u
#define SX_X_WAIT 0x12u
#define SX_X_SYNC 0x13u
#define SX_X_FENCE 0x14u
#define SX_X_MFSR 0x18u   // I: rd = sr[imm]
#define SX_X_MTSR 0x19u   // I: sr[imm] = ra
#define SX_X_XSR 0x1au    // I: rd = sr[imm], sr[imm] = ra
#define SX_X_SRSET 0x1bu  // I: rd = sr[imm], sr[imm] |= ra
#define SX_X_SRCLR 0x1cu  // I: rd = sr[imm], sr[imm] &= ~ra

// ---- Special registers
#define SX_SR_STATUS 0u   // bit 0 IE, 1 PIE, 2 PS (previous mode supervisor), 3 S (read only)
#define SX_SR_EPC 1u
#define SX_SR_CAUSE 2u
#define SX_SR_TVAL 3u
#define SX_SR_TVEC 4u
#define SX_SR_SCRATCH 5u
#define SX_SR_IEN 6u      // bit 0 software, 1 timer, 2 external
#define SX_SR_IPEND 7u
#define SX_SR_TCMP_LO 8u
#define SX_SR_TCMP_HI 9u
#define SX_SR_TIME_LO 10u // user-readable from here on
#define SX_SR_TIME_HI 11u
#define SX_SR_CYCLE 12u
#define SX_SR_COREID 13u
#define SX_SR_PLAYER 14u
#define SX_SR_RANDOM 15u
#define SX_SR_LEFT 16u
#define SX_SR_OPS 17u
#define SX_SR_COUNT 18u
#define SX_SR_USER_FIRST SX_SR_TIME_LO
#define SX_ST_IE 1u
#define SX_ST_PIE 2u
#define SX_ST_PS 4u
#define SX_ST_S 8u

// ---- Causes (CAUSE); an interrupt has bit 31 and the bit of IPEND
#define SX_CAUSE_ILLEGAL 1u
#define SX_CAUSE_BREAK 2u
#define SX_CAUSE_SYSCALL 3u
#define SX_CAUSE_LOAD 4u
#define SX_CAUSE_STORE 5u
#define SX_CAUSE_FETCH 6u
#define SX_CAUSE_PRIV 7u
#define SX_CAUSE_IRQ 0x80000000u
#define SX_IRQ_SOFT 0u
#define SX_IRQ_TIMER 1u
#define SX_IRQ_EXT 2u

// ---- The address space
#define SX_BOOT 0x00000000u      // read-only to the cores: the device tree at SX_DTB
#define SX_DTB 0x00001000u
#define SX_DEV 0x00010000u       // device memory
#define SX_MBOX 0x00c00000u      // the workers' mailboxes
#define SX_CONTROL 0x01000000u   // the GPU's control words
#define SX_RAM 0x02000000u       // RAM, to the state texture's end
#define SX_ROM 0x40000000u
#define SX_ROM_END 0x60000000u
#define SX_OPS 0xf0000000u       // operations: stored to, made by the commit
#define SX_OP_COPY 1u            // {kind, dst, src, bytes}: memmove
#define SX_OP_FILL 2u            // {kind, dst, word, bytes}
#define SX_OP_BLIT 3u            // and at SX_OPS + 16: {dst stride, src stride, bytes a row, rows}

// ---- ELF
#define EM_SX 0x5358
#define R_SX_NONE 0
#define R_SX_32 1
#define R_SX_REL32 2
#define R_SX_IMM32 3      // an op's long immediate: S + A
#define R_SX_PCIMM32 4    // lpc's long immediate: S + A - the bundle
#define R_SX_BR9 5        // control word [25:17]: (S + A - the bundle) / 16
#define R_SX_J18 6        // control word [31:14] (cc 0: the far j, call)
#define R_SX_IMM12 7      // op word [31:20]: S + A, which must fit
#define R_SX_TPREL32 8    // a long immediate: S + A, from the thread pointer
#define R_SX_RELATIVE 9

#endif
