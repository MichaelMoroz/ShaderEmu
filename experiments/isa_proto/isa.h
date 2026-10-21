/* The machine's instruction set: a slot of 16 bytes (one RAM texel, little-endian words X Y Z W) holds a main
 * op and an optional second op. ISA.md is the description; this header is the encoding, shared by the
 * translator (xlat.c), the reference machine (rvc_cpu --native) and the tick (rvc_lean_isa/tick.hlsl). */
#ifndef ISA_H
#define ISA_H
#include <stdint.h>

#define ISA_SLOT 16u
#define ISA_SINK 32u

/* X: the main op's control */
#define X_KIND      3u          /* 0 integer, 1 memory, 2 general (Z's OPG names it); 3 is illegal */
#define K_INT       0u
#define K_MEM       1u
#define K_GEN       2u
#define X_NEG       (1u << 2)   /* integer: the sum subtracts; memory: a store */
#define X_STORE     (1u << 2)
#define X_SEL_SHIFT 3           /* integer unit: sum and or xor sll srl sra cmp */
#define X_SEL       (7u << 3)
enum { SEL_SUM, SEL_AND, SEL_OR, SEL_XOR, SEL_SLL, SEL_SRL, SEL_SRA, SEL_CMP };
#define X_UNS       (1u << 6)   /* the comparison is unsigned */
#define X_EQ        (1u << 7)   /* the comparison is equality */
#define X_INV       (1u << 8)   /* the comparison is inverted */
#define X_BR        (1u << 9)   /* a branch on the comparison: to pc + W, or with X_IND to (A + W) & ~15 */
#define X_STEP      (1u << 10)  /* the branch steps its register first: rd = A + step, compared as stepped */
#define X_LD2       (1u << 11)  /* the second op is a word load from RF + RG + imm8 */
#define X_RG_SHIFT  12          /* the second op's second register */
#define X_SEL2_SHIFT 17         /* the second unit: sum and or xor sll srl sra sub */
#define X_SEL2      (7u << 17)
#define X_ST2       (1u << 20)  /* the second op stores RG to the word after the main store's */
#define X_ADDAND    (1u << 21)  /* the second op is (RF + imm8) & RG */
#define X_BIG       (1u << 22)  /* W is the second op's immediate; the main op's is 0 */
#define X_LINK      (1u << 23)  /* rd = pc + 16 */
#define X_IND       (1u << 24)  /* the branch's target is (A + W) & ~15 */
#define X_MW_SHIFT  25          /* memory width: 0 word, 1 byte, 2 half */
#define X_MW        (3u << 25)
#define X_MU        (1u << 27)  /* a narrow load is zero-extended */
#define X_PCA       (1u << 28)  /* A is the slot's own address, not a register */
#define X_ALW       (1u << 29)  /* the branch is always taken */
#define X_CNT_SHIFT 30          /* RISC-V instructions the slot does, 1 to 3; 0 is an illegal slot */

/* Y: ra 0-4, rf 5-9, re 10-15, imm8 24-31 (the second op's immediate, signed). A register written is 6 bits:
 * 32 is the sink, where a write to x0 goes; x0 always reads 0. */
#define Y_RA(y)   ((y) & 31u)
#define Y_RF(y)   (((y) >> 5) & 31u)
#define Y_RE(y)   (((y) >> 10) & 63u)
#define Y_IMM8(y) ((int32_t)(y) >> 24)

/* Z: rd 0-5, rb 6-10, step 12-15 (signed), OPG 16-23, rc 24-28 */
#define Z_RD(z)   ((z) & 63u)
#define Z_RB(z)   (((z) >> 6) & 31u)
#define Z_STEP(z) ((int32_t)((z) << 16) >> 28)
#define Z_OPG(z)  (((z) >> 16) & 255u)
#define Z_RC(z)   (((z) >> 24) & 31u)

/* General ops (K_GEN): registers rd, ra, rb, rc from Y and Z, f-registers where the op says, W an immediate */
enum {
    G_ILLEGAL = 0,
    G_MUL, G_MULH, G_MULHSU, G_MULHU, G_DIV, G_DIVU, G_REM, G_REMU,
    G_LR, G_SC, G_AMOSWAP, G_AMOADD, G_AMOXOR, G_AMOAND, G_AMOOR, G_AMOMIN, G_AMOMAX, G_AMOMINU, G_AMOMAXU,
    G_CSRRW, G_CSRRS, G_CSRRC, G_CSRRWI, G_CSRRSI, G_CSRRCI,   /* W: the CSR's number; ...I: ra is the immediate */
    G_ECALL, G_EBREAK, G_SRET, G_WFI, G_FENCEI, G_PAUSE, G_SFENCE,
    G_FLW, G_FSW,                                    /* f[rd] = mem[A + W]; mem[A + W] = f[rb] */
    G_FADD, G_FSUB, G_FMUL, G_FDIV, G_FSQRT,          /* W: the rounding mode (unused: the host's) */
    G_FSGNJ, G_FSGNJN, G_FSGNJX, G_FMIN, G_FMAX,
    G_FCVTWS, G_FCVTWUS, G_FCVTSW, G_FCVTSWU,        /* W: the rounding mode, 7 dynamic */
    G_FMVXW, G_FCLASS, G_FMVWX, G_FEQ, G_FLT, G_FLE,
    G_FMADD, G_FMSUB, G_FNMSUB, G_FNMADD,            /* f[rd] = +-f[ra] * f[rb] +- f[rc] */
    G_JMAP,   /* a jump to the RISC-V address in ra through the map at pc + W: (lo, n, n offsets from the map) */
    G_COUNT
};

static inline uint32_t isa_cnt(uint32_t x) { return x >> X_CNT_SHIFT; }

#endif
