/*
 * The 6502 recompiled on the machine (docs/nes.md): the cartridge's code is translated to
 * RISC-V instructions as it is first reached, and nofrendo's interpreter runs only what is
 * not translated. Included by nofrendo's nes6502.c (nofrendo.patch), whose statics it uses.
 *
 * A translated instruction does what the interpreter's macro for it does, in the same order,
 * and takes the same cycles: after each one the cycles left are tested, and code that runs
 * out leaves with the 6502's pc. So a run is the same with and without (NES_CPU=interp).
 *
 * Registers while translated code runs: s0 A, s1 X, s2 Y, s3 S, s4 and s5 the values N and Z
 * were last set from, s6 C (0 or 1), s7 V (zero or not), s8 the cycles left, s9 the RAM,
 * s10 the state below, s11 the routines (rc_rt). The other flags stay the interpreter's.
 */
#include <sys/mman.h>

typedef struct
{
   uint32 a, x, y, s, n, z, c, v;   /* 0 to 28 */
   int32 rem;                       /* 32: cycles left */
   uint32 pc;                       /* 36: where translated code left */
   int32 bias;                      /* 40: taken off rem by a write that must leave */
   uint8 **pages;                   /* 44: the 6502's sixteen pages */
   uint8 *prg;                      /* 48: the cartridge's program, and its size */
   uint32 size;                     /* 52 */
   uint32 *table;                   /* 56: by byte of the program: its code, or 0; bit 0: not an entry */
   uint8 *at;                       /* 60: by byte of the program: the page (pc >> 12) it was translated for */
   uint8 *ram;                      /* 64 */
} rc_state_t;

static rc_state_t rc;
static int rc_fixed;                /* the cartridge's pages never change: every jump is direct */
static uint32 *rc_code, *rc_p, *rc_end;
static unsigned rc_count, rc_runs, rc_entries;
static int rc_full;                 /* no room for more code: what is not translated stays so */

#define  RC_CODE_BYTES  0x100000    /* a jal reaches all of it */
#define  RC_BIAS        0x1000000

extern char rc_rt[], rc_rt_out[], rc_rt_read[], rc_rt_write[], rc_rt_dispatch[], rc_rt_idle[];
extern void rc_enter(uint32 code, rc_state_t *state, char *routines);

/* ---- what translated code calls: the routines, and the slow ways out of them ---- */

static void rc_flush(int rem)
{
   cpu.total_cycles += remaining_cycles - rem;
   remaining_cycles = rem;
}

/* a read that is neither RAM nor the cartridge: a device's, at the cycle the interpreter's is */
__attribute__((used)) uint32 rc_slow_read(uint32 address, int rem)
{
   uint32 value;

   rc_flush(rem);
   value = mem_readbyte(address);
   rc.rem = remaining_cycles;
   return value;
}

/* a write to a device or the cartridge: it may end the slice or turn pages, so the code leaves */
__attribute__((used)) void rc_slow_write(uint32 address, uint32 value, int rem)
{
   rc_flush(rem);
   mem_writebyte(address, value);
   rc.rem = remaining_cycles - RC_BIAS;
   rc.bias = RC_BIAS;
}

/* a branch back over a loop that only looks: the cycles of its turns left (idle_loop) */
__attribute__((used)) int rc_idle(uint32 where, uint32 a, uint32 x, uint32 y)
{
   uint8 p = (rc.n & N_FLAG) | (rc.v ? V_FLAG : 0) | (rc.z ? 0 : Z_FLAG) | (rc.c ? C_FLAG : 0);

   rc_flush(rc.rem);
   return idle_loop(where & 0xFFFF, where >> 16, a, x, y, p, remaining_cycles);
}

__asm__(
   ".pushsection .text\n"
   ".align 2\n"
   "rc_rt:\n"
   /* out: a0 is where the word with the 6502's pc is */
   "rc_rt_out:\n"
   "   lw a0, 0(a0)\n"
   "rc_rt_out_pc:\n"
   "   sw s0, 0(s10)\n   sw s1, 4(s10)\n   sw s2, 8(s10)\n   sw s3, 12(s10)\n"
   "   sw s4, 16(s10)\n  sw s5, 20(s10)\n  sw s6, 24(s10)\n  sw s7, 28(s10)\n"
   "   sw s8, 32(s10)\n  sw a0, 36(s10)\n"
   "   lw ra, 60(sp)\n   lw s0, 56(sp)\n   lw s1, 52(sp)\n   lw s2, 48(sp)\n"
   "   lw s3, 44(sp)\n   lw s4, 40(sp)\n   lw s5, 36(sp)\n   lw s6, 32(sp)\n"
   "   lw s7, 28(sp)\n   lw s8, 24(sp)\n   lw s9, 20(sp)\n   lw s10, 16(sp)\n"
   "   lw s11, 12(sp)\n  addi sp, sp, 64\n ret\n"
   /* read: a0 an address, then its byte; a2 and a3 are kept */
   "rc_rt_read:\n"
   "   srli t0, a0, 11\n   bnez t0, 1f\n"
   "   add t0, s9, a0\n    lbu a0, 0(t0)\n     ret\n"
   "1: srli t0, a0, 15\n   beqz t0, 2f\n"
   "   srli t0, a0, 12\n   slli t0, t0, 2\n    lw t1, 44(s10)\n    add t0, t0, t1\n"
   "   lw t0, 0(t0)\n      slli t1, a0, 20\n   srli t1, t1, 20\n   add t0, t0, t1\n"
   "   lbu a0, 0(t0)\n     ret\n"
   "2: addi sp, sp, -16\n  sw ra, 12(sp)\n     sw a2, 8(sp)\n      sw a3, 4(sp)\n"
   "   mv a1, s8\n         call rc_slow_read\n"
   "   lw ra, 12(sp)\n     lw a2, 8(sp)\n      lw a3, 4(sp)\n      addi sp, sp, 16\n"
   "   lw s8, 32(s10)\n    ret\n"
   /* write: a0 an address, a1 a byte */
   "rc_rt_write:\n"
   "   srli t0, a0, 11\n   bnez t0, 1f\n"
   "   add t0, s9, a0\n    sb a1, 0(t0)\n      ret\n"
   "1: addi sp, sp, -16\n  sw ra, 12(sp)\n"
   "   mv a2, s8\n         call rc_slow_write\n"
   "   lw ra, 12(sp)\n     addi sp, sp, 16\n"
   "   lw s8, 32(s10)\n    ret\n"
   /* dispatch: a0 the 6502's pc; to its code if it has an entry here, else out */
   "rc_rt_dispatch:\n"
   "   blez s8, rc_rt_out_pc\n"
   "   srli t0, a0, 12\n   slli t1, t0, 2\n    lw t2, 44(s10)\n    add t1, t1, t2\n"
   "   lw t1, 0(t1)\n      slli t2, a0, 20\n   srli t2, t2, 20\n   add t1, t1, t2\n"
   "   lw t2, 48(s10)\n    sub t1, t1, t2\n    lw t2, 52(s10)\n    bgeu t1, t2, rc_rt_out_pc\n"
   "   lw t2, 60(s10)\n    add t2, t2, t1\n    lbu t2, 0(t2)\n     bne t2, t0, rc_rt_out_pc\n"
   "   lw t2, 56(s10)\n    slli t1, t1, 2\n    add t2, t2, t1\n    lw t1, 0(t2)\n"
   "   andi t2, t1, 1\n    bnez t2, rc_rt_out_pc\n beqz t1, rc_rt_out_pc\n jr t1\n"
   /* idle: after a taken branch back; the word after the call is the loop's top and end */
   "rc_rt_idle:\n"
   "   blez s8, 9f\n"
   "   addi sp, sp, -16\n  sw ra, 12(sp)\n"
   "   sw s4, 16(s10)\n    sw s5, 20(s10)\n    sw s6, 24(s10)\n    sw s7, 28(s10)\n"
   "   sw s8, 32(s10)\n"
   "   lw a0, 0(ra)\n      mv a1, s0\n         mv a2, s1\n         mv a3, s2\n"
   "   call rc_idle\n"
   "   lw ra, 12(sp)\n     addi sp, sp, 16\n   sub s8, s8, a0\n"
   "9: addi ra, ra, 4\n    ret\n"
   /* in: a0 the code, a1 the state, a2 the routines */
   ".globl rc_enter\n"
   "rc_enter:\n"
   "   addi sp, sp, -64\n"
   "   sw ra, 60(sp)\n   sw s0, 56(sp)\n   sw s1, 52(sp)\n   sw s2, 48(sp)\n"
   "   sw s3, 44(sp)\n   sw s4, 40(sp)\n   sw s5, 36(sp)\n   sw s6, 32(sp)\n"
   "   sw s7, 28(sp)\n   sw s8, 24(sp)\n   sw s9, 20(sp)\n   sw s10, 16(sp)\n"
   "   sw s11, 12(sp)\n"
   "   mv t0, a0\n       mv s10, a1\n      mv s11, a2\n"
   "   lw s0, 0(s10)\n   lw s1, 4(s10)\n   lw s2, 8(s10)\n   lw s3, 12(s10)\n"
   "   lw s4, 16(s10)\n  lw s5, 20(s10)\n  lw s6, 24(s10)\n  lw s7, 28(s10)\n"
   "   lw s8, 32(s10)\n  lw s9, 64(s10)\n"
   "   jr t0\n"
   ".popsection\n"
);

/* ---- the assembler ---- */

enum { ZERO = 0, RA = 1, T0 = 5, T1 = 6, T2 = 7, A0 = 10, A1 = 11, A2 = 12, A3 = 13,
       RA_ = 8 /* A */, RX = 9, RY = 18, RS = 19, RN = 20, RZ = 21, RC = 22, RV = 23,
       REM = 24, RAM = 25, RT = 27 };

#define  E(word)                 (*rc_p++ = (uint32) (word))
#define  E_R(f7, rs2, rs1, f3, rd)  E((f7) << 25 | (rs2) << 20 | (rs1) << 15 | (f3) << 12 | (rd) << 7 | 0x33)
#define  E_I(opc, f3, rd, rs1, imm) E(((uint32) (imm) & 0xFFF) << 20 | (rs1) << 15 | (f3) << 12 | (rd) << 7 | (opc))
#define  e_add(rd, a, b)         E_R(0, b, a, 0, rd)
#define  e_sub(rd, a, b)         E_R(0x20, b, a, 0, rd)
#define  e_and(rd, a, b)         E_R(0, b, a, 7, rd)
#define  e_or(rd, a, b)          E_R(0, b, a, 6, rd)
#define  e_xor(rd, a, b)         E_R(0, b, a, 4, rd)
#define  e_sltu(rd, a, b)        E_R(0, b, a, 3, rd)
#define  e_addi(rd, a, imm)      E_I(0x13, 0, rd, a, imm)
#define  e_xori(rd, a, imm)      E_I(0x13, 4, rd, a, imm)
#define  e_andi(rd, a, imm)      E_I(0x13, 7, rd, a, imm)
#define  e_slli(rd, a, n)        E_I(0x13, 1, rd, a, n)
#define  e_srli(rd, a, n)        E_I(0x13, 5, rd, a, n)
#define  e_mv(rd, a)             e_addi(rd, a, 0)
#define  e_lbu(rd, base, off)    E_I(0x03, 4, rd, base, off)
#define  e_jalr(rd, base, off)   E_I(0x67, 0, rd, base, off)
#define  e_sb(src, base, off)    E(((uint32) (off) & 0xFE0) << 20 | (src) << 20 | (base) << 15 | ((uint32) (off) & 0x1F) << 7 | 0x23)

static uint32 rc_branch_word(int f3, int a, int b, int32 off)
{
   uint32 o = (uint32) off;

   return (o & 0x1000) << 19 | (o & 0x7E0) << 20 | b << 20 | a << 15 | f3 << 12
          | (o & 0x1E) << 7 | (o & 0x800) >> 4 | 0x63;
}

static uint32 rc_jal_word(int rd, int32 off)
{
   uint32 o = (uint32) off;

   return (o & 0x100000) << 11 | (o & 0x7FE) << 20 | (o & 0x800) << 9 | (o & 0xFF000) | rd << 7 | 0x6F;
}

#define  e_beqz(a, off)          E(rc_branch_word(0, a, ZERO, off))
#define  e_bnez(a, off)          E(rc_branch_word(1, a, ZERO, off))
#define  e_bgtz(a, off)          E(rc_branch_word(4, ZERO, a, off))

static void e_li(int rd, uint32 value)
{
   if (value < 0x800)
      e_addi(rd, ZERO, value);
   else
   {
      E(((value + 0x800) & 0xFFFFF000) | rd << 7 | 0x37);
      if (value & 0xFFF)
         e_addi(rd, rd, value & 0xFFF);
   }
}

#define  RT_OFF(routine)         ((int) ((routine) - rc_rt))
#define  e_call(routine)         e_jalr(RA, RT, RT_OFF(routine))

/* leave with the 6502 at pc */
static void e_out(uint32 pc)
{
   e_jalr(A0, RT, RT_OFF(rc_rt_out));
   E(pc);
}

/* an instruction's end: its cycles, and out if none are left */
static void e_tail(int cycles, uint32 next)
{
   e_addi(REM, REM, -cycles);
   e_bgtz(REM, 12);
   e_out(next);
}

#define  e_nz(reg)               (e_mv(RN, reg), e_mv(RZ, reg))

/* ---- the 6502's instructions ---- */

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_INX, M_INY, M_REL };
enum { K_NONE, K_INTERP1, K_LDA, K_LDX, K_LDY, K_STA, K_STX, K_STY, K_ADC, K_SBC, K_AND, K_ORA,
       K_EOR, K_CMP, K_CPX, K_CPY, K_BIT, K_INC, K_DEC, K_ASL, K_LSR, K_ROL, K_ROR, K_INX, K_INY,
       K_DEX, K_DEY, K_TAX, K_TAY, K_TXA, K_TYA, K_TSX, K_TXS, K_PHA, K_PLA, K_CLC, K_SEC, K_CLV,
       K_NOP, K_BRANCH, K_JMP, K_JSR, K_RTS };

typedef struct { uint8 kind, mode, cycles; } rc_op_t;

/* (not here, so the interpreter's: brk, rti, jmp (), the flags i and d, php, plp, all undocumented) */
static const rc_op_t rc_ops[256] =
{
#define  ALU(base, k) \
   [base + 0x01] = { k, M_INX, 6 }, [base + 0x05] = { k, M_ZP, 3 }, [base + 0x09] = { k, M_IMM, 2 }, \
   [base + 0x0D] = { k, M_ABS, 4 }, [base + 0x11] = { k, M_INY, 5 }, [base + 0x15] = { k, M_ZPX, 4 }, \
   [base + 0x19] = { k, M_ABY, 4 }, [base + 0x1D] = { k, M_ABX, 4 }
#define  SHIFT(base, k) \
   [base + 0x06] = { k, M_ZP, 5 }, [base + 0x0A] = { k, M_ACC, 2 }, [base + 0x0E] = { k, M_ABS, 6 }, \
   [base + 0x16] = { k, M_ZPX, 6 }, [base + 0x1E] = { k, M_ABX, 7 }
#define  STEP(base, k) \
   [base + 0x06] = { k, M_ZP, 5 }, [base + 0x0E] = { k, M_ABS, 6 }, \
   [base + 0x16] = { k, M_ZPX, 6 }, [base + 0x1E] = { k, M_ABX, 7 }
   ALU(0x00, K_ORA), ALU(0x20, K_AND), ALU(0x40, K_EOR), ALU(0x60, K_ADC),
   ALU(0xA0, K_LDA), ALU(0xC0, K_CMP), ALU(0xE0, K_SBC),
   [0x81] = { K_STA, M_INX, 6 }, [0x85] = { K_STA, M_ZP, 3 }, [0x8D] = { K_STA, M_ABS, 4 },
   [0x91] = { K_STA, M_INY, 6 }, [0x95] = { K_STA, M_ZPX, 4 }, [0x99] = { K_STA, M_ABY, 5 },
   [0x9D] = { K_STA, M_ABX, 5 },
   SHIFT(0x00, K_ASL), SHIFT(0x20, K_ROL), SHIFT(0x40, K_LSR), SHIFT(0x60, K_ROR),
   STEP(0xC0, K_DEC), STEP(0xE0, K_INC),
   [0x86] = { K_STX, M_ZP, 3 }, [0x8E] = { K_STX, M_ABS, 4 }, [0x96] = { K_STX, M_ZPY, 4 },
   [0x84] = { K_STY, M_ZP, 3 }, [0x8C] = { K_STY, M_ABS, 4 }, [0x94] = { K_STY, M_ZPX, 4 },
   [0xA2] = { K_LDX, M_IMM, 2 }, [0xA6] = { K_LDX, M_ZP, 3 }, [0xAE] = { K_LDX, M_ABS, 4 },
   [0xB6] = { K_LDX, M_ZPY, 4 }, [0xBE] = { K_LDX, M_ABY, 4 },
   [0xA0] = { K_LDY, M_IMM, 2 }, [0xA4] = { K_LDY, M_ZP, 3 }, [0xAC] = { K_LDY, M_ABS, 4 },
   [0xB4] = { K_LDY, M_ZPX, 4 }, [0xBC] = { K_LDY, M_ABX, 4 },
   [0xC0] = { K_CPY, M_IMM, 2 }, [0xC4] = { K_CPY, M_ZP, 3 }, [0xCC] = { K_CPY, M_ABS, 4 },
   [0xE0] = { K_CPX, M_IMM, 2 }, [0xE4] = { K_CPX, M_ZP, 3 }, [0xEC] = { K_CPX, M_ABS, 4 },
   [0x24] = { K_BIT, M_ZP, 3 }, [0x2C] = { K_BIT, M_ABS, 4 },
   [0xE8] = { K_INX, M_IMP, 2 }, [0xC8] = { K_INY, M_IMP, 2 }, [0xCA] = { K_DEX, M_IMP, 2 },
   [0x88] = { K_DEY, M_IMP, 2 }, [0xAA] = { K_TAX, M_IMP, 2 }, [0xA8] = { K_TAY, M_IMP, 2 },
   [0x8A] = { K_TXA, M_IMP, 2 }, [0x98] = { K_TYA, M_IMP, 2 }, [0xBA] = { K_TSX, M_IMP, 2 },
   [0x9A] = { K_TXS, M_IMP, 2 }, [0x48] = { K_PHA, M_IMP, 3 }, [0x68] = { K_PLA, M_IMP, 4 },
   [0x18] = { K_CLC, M_IMP, 2 }, [0x38] = { K_SEC, M_IMP, 2 }, [0xB8] = { K_CLV, M_IMP, 2 },
   [0xEA] = { K_NOP, M_IMP, 2 },
   [0x10] = { K_BRANCH, M_REL, 2 }, [0x30] = { K_BRANCH, M_REL, 2 }, [0x50] = { K_BRANCH, M_REL, 2 },
   [0x70] = { K_BRANCH, M_REL, 2 }, [0x90] = { K_BRANCH, M_REL, 2 }, [0xB0] = { K_BRANCH, M_REL, 2 },
   [0xD0] = { K_BRANCH, M_REL, 2 }, [0xF0] = { K_BRANCH, M_REL, 2 },
   [0x4C] = { K_JMP, M_ABS, 3 }, [0x20] = { K_JSR, M_ABS, 6 }, [0x60] = { K_RTS, M_IMP, 6 },
   [0x08] = { K_INTERP1 }, [0x28] = { K_INTERP1 }, [0x58] = { K_INTERP1 }, [0x78] = { K_INTERP1 },
   [0xD8] = { K_INTERP1 }, [0xF8] = { K_INTERP1 },
#undef   ALU
#undef   SHIFT
#undef   STEP
};

static const uint8 rc_length[12] = { 1, 1, 2, 2, 2, 2, 3, 3, 3, 2, 2, 2 };

/* ---- translation ---- */

#define  RC_WORK  4096

typedef struct { uint32 *site; uint32 pc; } rc_fix_t;

static uint32 rc_work[RC_WORK];
static rc_fix_t rc_fix[RC_WORK];
static int rc_works, rc_fixes;

/* where in the program the 6502's address is now, or past its end */
static uint32 rc_where(uint32 pc)
{
   if (pc < 0x8000 || pc > 0xFFFF)
      return 0xFFFFFFFF;
   return (uint32) (cpu.mem_page[pc >> NES6502_BANKSHIFT] + (pc & NES6502_BANKMASK) - rc.prg);
}

/* the cycle a read that crosses a page takes: (uint8) (low + index) < index */
static void e_cross(int index, uint32 low)
{
   e_addi(T1, index, low & 0xFF);
   e_srli(T1, T1, 8);
   e_sub(REM, REM, T1);
}

/* the address of an operand in a0 (the modes whose address is found as the code runs) */
static void e_address(int mode, uint32 arg, int penalty)
{
   int index = (M_ABX == mode) ? RX : RY;

   switch (mode)
   {
   case M_ABS:
      e_li(A0, arg);
      break;
   case M_ABX: case M_ABY:
      e_li(A0, arg);
      e_add(A0, A0, index);
      if (arg > 0xFF00)
      {
         e_slli(A0, A0, 16);
         e_srli(A0, A0, 16);
      }
      if (penalty)
         e_cross(index, arg);
      break;
   case M_INX:
      e_addi(T0, RX, arg);
      e_andi(T0, T0, 0xFF);
      e_add(T0, T0, RAM);
      e_lbu(A0, T0, 0);
      e_lbu(T1, T0, 1);
      e_slli(T1, T1, 8);
      e_or(A0, A0, T1);
      break;
   case M_INY:
      e_lbu(A0, RAM, arg);
      e_lbu(T1, RAM, arg + 1);
      e_slli(T1, T1, 8);
      e_or(A0, A0, T1);
      e_add(A0, A0, RY);
      e_slli(A0, A0, 16);
      e_srli(A0, A0, 16);
      if (penalty)
      {
         e_andi(T1, A0, 0xFF);
         e_sltu(T1, T1, RY);
         e_sub(REM, REM, T1);
      }
      break;
   }
}

/* whether a mode's operand is always RAM: then it is read and written in place */
static int rc_in_ram(int mode, uint32 arg)
{
   return M_ZP == mode || M_ZPX == mode || M_ZPY == mode || (M_ABS == mode && arg < 0x800)
          || ((M_ABX == mode || M_ABY == mode) && arg + 0xFF < 0x800);
}

/* for an operand in RAM: a register and an offset that are its place (t0 may be used) */
static int e_ram_place(int mode, uint32 arg, int *offset)
{
   int index = (M_ZPX == mode || M_ABX == mode) ? RX : RY;

   *offset = 0;
   switch (mode)
   {
   case M_ZP: case M_ABS:
      *offset = arg;
      return RAM;
   case M_ZPX: case M_ZPY:
      e_addi(T0, index, arg);
      e_andi(T0, T0, 0xFF);
      e_add(T0, T0, RAM);
      return T0;
   default: /* abs,x and abs,y below the RAM's end */
      e_add(T0, RAM, index);
      *offset = arg;
      return T0;
   }
}

/* an operand's value into a register */
static void e_load(int mode, uint32 arg, int penalty, int to)
{
   int base, offset;

   if (M_IMM == mode)
      e_li(to, arg);
   else if (rc_in_ram(mode, arg))
   {
      if (penalty && (M_ABX == mode || M_ABY == mode))
         e_cross(M_ABX == mode ? RX : RY, arg);
      base = e_ram_place(mode, arg, &offset);
      e_lbu(to, base, offset);
   }
   else
   {
      e_address(mode, arg, penalty);
      e_call(rc_rt_read);
      if (to != A0)
         e_mv(to, A0);
   }
}

static void e_store(int mode, uint32 arg, int from)
{
   int base, offset;

   if (rc_in_ram(mode, arg))
   {
      base = e_ram_place(mode, arg, &offset);
      e_sb(from, base, offset);
   }
   else
   {
      e_address(mode, arg, 0);
      e_mv(A1, from);
      e_call(rc_rt_write);
   }
}

/* asl, lsr, rol, ror, inc, dec of a register (t1 is used) */
static void e_modify(int kind, int reg)
{
   switch (kind)
   {
   case K_ASL:
      e_srli(RC, reg, 7);
      e_slli(reg, reg, 1);
      e_andi(reg, reg, 0xFF);
      break;
   case K_LSR:
      e_andi(RC, reg, 1);
      e_srli(reg, reg, 1);
      break;
   case K_ROL:
      e_srli(T1, reg, 7);
      e_slli(reg, reg, 1);
      e_or(reg, reg, RC);
      e_andi(reg, reg, 0xFF);
      e_mv(RC, T1);
      break;
   case K_ROR:
      e_slli(T1, RC, 7);
      e_andi(RC, reg, 1);
      e_srli(reg, reg, 1);
      e_or(reg, reg, T1);
      break;
   case K_INC:
      e_addi(reg, reg, 1);
      e_andi(reg, reg, 0xFF);
      break;
   case K_DEC:
      e_addi(reg, reg, -1);
      e_andi(reg, reg, 0xFF);
      break;
   }
   e_nz(reg);
}

static void e_compare(int reg)
{
   e_sub(T1, reg, A0);
   e_srli(T0, T1, 8);
   e_andi(T0, T0, 1);
   e_xori(RC, T0, 1);
   e_andi(RN, T1, 0xFF);
   e_mv(RZ, RN);
}

static void e_push(int reg)
{
   e_add(T0, RAM, RS);
   e_sb(reg, T0, 0x100);
   e_addi(RS, RS, -1);
   e_andi(RS, RS, 0xFF);
}

static void e_pull(int reg)
{
   e_addi(RS, RS, 1);
   e_andi(RS, RS, 0xFF);
   e_add(T0, RAM, RS);
   e_lbu(reg, T0, 0x100);
}

/* whether a loop is one idle_loop could pass over: loads, compares, tests and branches only */
static int rc_idle_possible(uint32 top, uint32 end)
{
   uint32 pc = top;

   while (pc < end)
   {
      uint8 op = bank_readbyte(pc);
      const rc_op_t *info = &rc_ops[op];

      if (K_BRANCH == info->kind)
         pc += 2;
      else if (0xEA == op || 0x18 == op || 0x38 == op)
         pc += 1;
      else if ((K_LDA == info->kind || K_LDX == info->kind || K_LDY == info->kind || K_CMP == info->kind
                || K_CPX == info->kind || K_CPY == info->kind || K_BIT == info->kind || K_AND == info->kind
                || K_ORA == info->kind || K_EOR == info->kind)
               && (M_IMM == info->mode || M_ZP == info->mode || M_ABS == info->mode))
      {
         if (M_ABS == info->mode)
         {
            uint32 address = bank_readbyte(pc + 1) | bank_readbyte(pc + 2) << 8;

            if (address >= 0x800 && address < 0x8000 && 0x2002 != address)
               return 0;
         }
         pc += rc_length[info->mode];
      }
      else
         return 0;
   }
   return pc == end;
}

/* a jump to the code of the 6502's address: straight there when the pages between cannot
** have turned (the same page, or a cartridge that turns none), else through the dispatch */
static void e_jump(uint32 from, uint32 target)
{
   uint32 where = rc_where(target);

   if (where < rc.size && (rc_fixed || (target >> 12) == (from >> 12))
       && (0 == rc.table[where] || rc.at[where] == (target >> 12)))
   {
      if (rc.table[where])
      {
         E(rc_jal_word(ZERO, (int32) ((rc.table[where] & ~1u) - (uint32) rc_p)));
         return;
      }
      if (rc_works < RC_WORK && rc_fixes < RC_WORK)
      {
         rc_work[rc_works++] = target;
         rc_fix[rc_fixes].site = rc_p;
         rc_fix[rc_fixes++].pc = target;
         E(0);
         return;
      }
   }
   e_li(A0, target);
   e_jalr(ZERO, RT, RT_OFF(rc_rt_dispatch));
}

/* out if no cycles are left, as the interpreter looks after every instruction; then the jump */
static void e_leave_to(uint32 from, uint32 target)
{
   e_bgtz(REM, 12);
   e_out(target);
   e_jump(from, target);
}

static void rc_instruction(uint32 pc, uint8 op, uint32 arg)
{
   const rc_op_t *info = &rc_ops[op];
   int mode = info->mode, kind = info->kind;
   uint32 next = pc + rc_length[mode];
   int reg, base, offset;

   switch (kind)
   {
   case K_LDA: case K_LDX: case K_LDY:
      reg = (K_LDA == kind) ? RA_ : (K_LDX == kind) ? RX : RY;
      e_load(mode, arg, 1, reg);
      e_nz(reg);
      break;
   case K_STA: case K_STX: case K_STY:
      e_store(mode, arg, (K_STA == kind) ? RA_ : (K_STX == kind) ? RX : RY);
      break;
   case K_ADC:
      e_load(mode, arg, 1, A0);
      e_add(T1, RA_, A0);
      e_add(T1, T1, RC);
      e_xor(T2, RA_, A0);
      e_xori(T2, T2, -1);
      e_xor(T0, RA_, T1);
      e_and(T2, T2, T0);
      e_andi(RV, T2, 0x80);
      e_srli(RC, T1, 8);
      e_andi(RA_, T1, 0xFF);
      e_nz(RA_);
      break;
   case K_SBC:
      e_load(mode, arg, 1, A0);
      e_sub(T1, RA_, A0);
      e_xori(T0, RC, 1);
      e_sub(T1, T1, T0);
      e_xor(T2, RA_, A0);
      e_xor(T0, RA_, T1);
      e_and(T2, T2, T0);
      e_andi(RV, T2, 0x80);
      e_srli(T0, T1, 8);
      e_andi(T0, T0, 1);
      e_xori(RC, T0, 1);
      e_andi(RA_, T1, 0xFF);
      e_nz(RA_);
      break;
   case K_AND: case K_ORA: case K_EOR:
      e_load(mode, arg, 1, A0);
      if (K_AND == kind)
         e_and(RA_, RA_, A0);
      else if (K_ORA == kind)
         e_or(RA_, RA_, A0);
      else
         e_xor(RA_, RA_, A0);
      e_nz(RA_);
      break;
   case K_CMP: case K_CPX: case K_CPY:
      e_load(mode, arg, 1, A0);
      e_compare((K_CMP == kind) ? RA_ : (K_CPX == kind) ? RX : RY);
      break;
   case K_BIT:
      e_load(mode, arg, 0, A0);
      e_mv(RN, A0);
      e_andi(RV, A0, 0x40);
      e_and(RZ, A0, RA_);
      break;
   case K_INC: case K_DEC: case K_ASL: case K_LSR: case K_ROL: case K_ROR:
      if (M_ACC == mode)
         e_modify(kind, RA_);
      else if (rc_in_ram(mode, arg))
      {
         base = e_ram_place(mode, arg, &offset);
         if (T0 == base)
         {
            e_mv(A3, T0);
            base = A3;
         }
         e_lbu(A0, base, offset);
         e_modify(kind, A0);
         e_sb(A0, base, offset);
      }
      else
      {
         e_address(mode, arg, 0);
         e_mv(A2, A0);
         e_call(rc_rt_read);
         e_modify(kind, A0);
         e_mv(A1, A0);
         e_mv(A0, A2);
         e_call(rc_rt_write);
      }
      break;
   case K_INX: e_modify(K_INC, RX); break;
   case K_INY: e_modify(K_INC, RY); break;
   case K_DEX: e_modify(K_DEC, RX); break;
   case K_DEY: e_modify(K_DEC, RY); break;
   case K_TAX: e_mv(RX, RA_); e_nz(RX); break;
   case K_TAY: e_mv(RY, RA_); e_nz(RY); break;
   case K_TXA: e_mv(RA_, RX); e_nz(RA_); break;
   case K_TYA: e_mv(RA_, RY); e_nz(RA_); break;
   case K_TSX: e_mv(RX, RS); e_nz(RX); break;
   case K_TXS: e_mv(RS, RX); break;
   case K_PHA: e_push(RA_); break;
   case K_PLA: e_pull(RA_); e_nz(RA_); break;
   case K_CLC: e_mv(RC, ZERO); break;
   case K_SEC: e_addi(RC, ZERO, 1); break;
   case K_CLV: e_mv(RV, ZERO); break;
   case K_NOP: break;

   case K_BRANCH:
   {
      static const uint8 flag_reg[4] = { T0, RV, RC, RZ };
      int which = op >> 6, when_set = (op >> 5) & 1;
      int32 back = (int8) arg;
      uint32 target = (next + back) & 0xFFFF, *skip;
      int taken = (((int) back + (int) (next & 0xFF)) & 0x100) ? 4 : 3;

      if (0 == which)
         e_andi(T0, RN, 0x80);
      /* (the Z register is zero when the flag is set) */
      skip = rc_p;
      E(0);
      e_addi(REM, REM, -taken);
      if (back <= -4 && back >= -16 && rc_idle_possible(target, next))
      {
         e_call(rc_rt_idle);
         E(target | next << 16);
      }
      e_leave_to(pc, target);
      *skip = rc_branch_word((when_set != (3 == which)) ? 0 : 1, flag_reg[which], ZERO,
                             (int32) ((uint32) rc_p - (uint32) skip));
      e_tail(2, next);
      return;
   }
   case K_JMP:
      e_addi(REM, REM, -3);
      e_leave_to(pc, arg);
      return;
   case K_JSR:
      e_li(T1, (pc + 2) >> 8);
      e_push(T1);
      e_li(T1, (pc + 2) & 0xFF);
      e_push(T1);
      e_addi(REM, REM, -6);
      e_leave_to(pc, arg);
      return;
   case K_RTS:
      e_pull(A0);
      e_pull(T1);
      e_slli(T1, T1, 8);
      e_or(A0, A0, T1);
      e_addi(A0, A0, 1);
      e_addi(REM, REM, -6);
      e_jalr(ZERO, RT, RT_OFF(rc_rt_dispatch));
      return;
   }
   e_tail(info->cycles, next);
}

/* Translates what is reached from the 6502's pc by jumps that are straight ones. */
static void rc_translate(uint32 start)
{
   int i;

   rc_works = rc_fixes = 0;
   rc_work[rc_works++] = start;
   rc_runs++;

   while (rc_works)
   {
      uint32 pc = rc_work[--rc_works];
      int falls = 0;

      for (;;)
      {
         uint32 where = rc_where(pc), arg, code = (uint32) rc_p;
         const rc_op_t *info;
         uint8 op;

         /* (room: the longest instruction, and what the jumps still to mend may need) */
         if (where >= rc.size || rc_p + 64 + 2 * rc_fixes > rc_end)
         {
            if (where < rc.size)
               rc_full = 1;
            if (falls)
               e_out(pc);
            break;
         }
         if (rc.table[where])
         {
            if (falls && rc.at[where] == (pc >> 12))
               E(rc_jal_word(ZERO, (int32) ((rc.table[where] & ~1u) - (uint32) rc_p)));
            else if (falls)
               e_out(pc);
            break;
         }
         op = bank_readbyte(pc);
         info = &rc_ops[op];
         rc.at[where] = pc >> 12;
         if (K_NONE == info->kind || K_INTERP1 == info->kind)
         {
            /* the interpreter's: fallen into, the code leaves; never entered */
            rc.table[where] = code | 1;
            e_out(pc);
            if (K_NONE == info->kind)
               break;
            pc += 1;
            falls = 0;
            continue;
         }
         arg = bank_readbyte(pc + 1);
         if (3 == rc_length[info->mode])
            arg |= bank_readbyte(pc + 2) << 8;
         rc.table[where] = code;
         rc_instruction(pc, op, arg);
         rc_count++;
         if (K_JMP == info->kind || K_RTS == info->kind)
            break;
         pc += rc_length[info->mode];
         falls = 1;
      }
   }

   /* the jumps to code that was not there yet */
   for (i = 0; i < rc_fixes; i++)
   {
      uint32 where = rc_where(rc_fix[i].pc), code = rc.table[where] & ~1u;

      if (0 == code || rc.at[where] != (rc_fix[i].pc >> 12))
      {
         code = (uint32) rc_p;
         e_out(rc_fix[i].pc);
      }
      *rc_fix[i].site = rc_jal_word(ZERO, (int32) (code - (uint32) rc_fix[i].site));
   }

   /* what was written is fetched as it was until the machine's pass ends: fence.i ends it */
   __asm__ volatile(".word 0x0000100f");
}

/* The interpreter asks before every instruction of its own: runs the 6502 in translated code
** from pc if there is any (translating first what was never looked at). True: it ran. */
#define  RC_RUN(ran) \
{ \
   ran = 0; \
   if (rc.table) \
   { \
      uint32 rc_at = rc_where(PC); \
      if (rc_at < rc.size) \
      { \
         if (0 == rc.table[rc_at] && !rc_full) \
            rc_translate(PC); \
         if (0 == (rc.table[rc_at] & 1) && rc.table[rc_at] && rc.at[rc_at] == (PC >> 12)) \
         { \
            rc.a = A; rc.x = X; rc.y = Y; rc.s = S; \
            rc.n = n_flag; rc.z = z_flag; rc.c = c_flag; rc.v = v_flag; \
            rc.rem = remaining_cycles; \
            rc.ram = ram; \
            rc_entries++; \
            rc_enter(rc.table[rc_at], &rc, rc_rt); \
            A = rc.a; X = rc.x; Y = rc.y; S = rc.s; \
            n_flag = rc.n; z_flag = rc.z; c_flag = rc.c; v_flag = rc.v; \
            PC = rc.pc; \
            rc_flush(rc.rem + rc.bias); \
            rc.bias = 0; \
            ran = 1; \
         } \
      } \
   } \
}

/* Called once the cartridge is in: its program, and whether its pages ever turn. */
void nes6502_recompile(uint8 *prg, int bytes, int fixed)
{
   rc_code = mmap(NULL, RC_CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (MAP_FAILED == (void *) rc_code)
      return;
   rc_p = rc_code;
   rc_end = rc_code + RC_CODE_BYTES / 4;
   rc.at = calloc(bytes, 1);
   rc.prg = prg;
   rc.size = bytes;
   rc.pages = cpu.mem_page;
   rc_fixed = fixed;
   rc.table = calloc(bytes, sizeof(uint32));
}

void nes6502_recompiled(unsigned *instructions, unsigned *bytes, unsigned *runs, unsigned *entries)
{
   *instructions = rc_count;
   *bytes = (unsigned) ((char *) rc_p - (char *) rc_code);
   *runs = rc_runs;
   *entries = rc_entries;
}
