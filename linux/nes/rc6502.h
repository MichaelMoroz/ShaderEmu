/*
 * The 6502 recompiled on the machine (docs/games/nes.md): the cartridge's code is translated to
 * RISC-V instructions as it is first reached, and nofrendo's interpreter runs only what is
 * not translated. Included by nofrendo's nes6502.c (nofrendo.patch), whose statics it uses.
 *
 * A translated instruction does what the interpreter's macro for it does, in the same order,
 * and takes the same cycles: after each one the cycles left are tested, and code that runs
 * out leaves with the 6502's pc. So a run is the same with and without (NES_CPU=interp).
 *
 * Registers while translated code runs: s0 A, s1 X, s2 Y, s3 S, s4 N and Z (below), s5 -1, s6 C
 * (0 or 1), s7 V (zero or not), s8 the cycles left, s9 the RAM, s10 the state below, s11 the
 * routines (rc_rt). The other flags stay the interpreter's.
 * N and Z are one value: Z is set when its low byte is zero, N when bit 7 or 8 is set. A result
 * is that value as it is; only bit, whose N is not its Z's, sets bit 8 (RC_NZ, RC_N, RC_Z).
 */
#include <sys/mman.h>

#define  RC_NZ(n, z)    ((((n) & 0x80) << 1) | ((z) ? 1 : 0))   /* from the interpreter's two */
#define  RC_N(nz)       (((nz) & 0x180) ? 0x80 : 0)
#define  RC_Z(nz)       ((nz) & 0xFF)

typedef struct
{
   uint32 a, x, y, s, n, z, c, v;   /* 0 to 28; n is N and Z, z is -1 */
   int32 rem;                       /* 32: cycles left */
   uint32 pc;                       /* 36: where translated code left */
   int32 bias;                      /* 40: taken off rem by a write that must leave */
   uint8 **pages;                   /* 44: the 6502's sixteen pages */
   uint8 *prg;                      /* 48: the cartridge's program, and its size */
   uint32 size;                     /* 52 */
   uint32 *table;                   /* 56: by byte of the program: its code, or 0; bit 0: not an entry */
   uint8 *at;                       /* 60: by byte of the program: the page (pc >> 12) it was translated for */
   uint8 *ram;                      /* 64 */
   uint32 paged_read;               /* 68: from here up a read is of the pages, no device's */
   uint32 paged_write;              /* 72: from here to 0x7FFF a write is to the pages */
   uint32 *direct;                  /* 76: by the 6502's address: its code (a cartridge whose pages never turn) */
} rc_state_t;

static rc_state_t rc;
static int rc_fixed;                /* the cartridge's pages never change: every jump is direct */
int rc_plain_runs_off;              /* NES_RC=plain: no unchecked copies of runs, to compare with */
int rc_eager;                       /* NES_RC=eager: a jsr's routine translated with the jsr, as it was */
extern int ppu_settle_write;
static uint32 *rc_code, *rc_p, *rc_end;
static unsigned rc_count, rc_runs, rc_entries;
static int rc_full;                 /* no room for more code: what is not translated stays so */

#define  RC_CODE_BYTES  0x100000    /* a jal reaches all of it */
#define  RC_BIAS        0x1000000

extern char rc_rt[], rc_rt_out[], rc_rt_read[], rc_rt_write[], rc_rt_dispatch[], rc_rt_direct[], rc_rt_idle[];
extern char rc_rt_rti[], rc_rt_vdata[], rc_rt_pad[], rc_rt_flag[], rc_rt_mapper[];
extern void rc_enter(uint32 code, rc_state_t *state, char *routines);

/* ---- what translated code calls: the routines, and the slow ways out of them ---- */

static void rc_flush(int rem)
{
   cpu.total_cycles += remaining_cycles - rem;
   remaining_cycles = rem;
}

/* (the machine's own registers are the first handlers of nofrendo's list, whatever the
** cartridge adds: theirs are called without the search) */
extern uint8 ppu_read(uint32 address), ppu_readhigh(uint32 address);
extern void ppu_write(uint32 address, uint8 value), ppu_writehigh(uint32 address, uint8 value);
extern void apu_write(uint32 address, uint8 value);

static int rc_slice_from;   /* the cycles so far, as the slice began */
extern int ppu_lines_ahead;
extern int ppu_lines_settle(int t, int *passed);

/* A slice of several lines (ppu_lines_ahead): those that have passed are counted and the
** slice is cut to the line it is in, before anything looks at a line or changes what one is.
** late: cycles the instruction has taken already. Returns what the slice was cut by. */
static inline __attribute__((always_inline)) int rc_settle(int late)
{
   int passed, cut;

   if (0 == ppu_lines_ahead)
      return 0;
   cut = ppu_lines_settle(cpu.total_cycles - rc_slice_from - late, &passed);
   rc_slice_from += passed;
   remaining_cycles -= cut;
   return cut;
}

/* a read that is neither RAM nor the cartridge: a device's, at the cycle the interpreter's is
** (crossed: the cycle a read across a page has taken by now) */
__attribute__((used)) uint32 rc_slow_read(uint32 address, int rem, int crossed)
{
   uint32 value;

   rc_flush(rem);
   if (address - 0x4016 < 2)
      value = ppu_readhigh(address);   /* (the pads: no line's business) */
   else if (rc_settle(crossed), address - 0x2000 < 0x2000)
      value = ppu_read(address);
   else
      value = mem_readbyte(address);
   rc.rem = remaining_cycles;
   return value;
}

/* A write to the cartridge (0x8000 up) by its handler, the first of the list whose range has it:
** kept while no handler before it overlaps its range. */
static nes6502_memwrite *rc_mapper_at;

static __attribute__((noinline)) void rc_mapper(uint32 address, uint8 value)
{
   nes6502_memwrite *mw = rc_mapper_at, *before;

   if (NULL == mw || address < mw->min_range || address > mw->max_range)
   {
      for (mw = cpu.write_handler; mw->min_range != 0xFFFFFFFF; mw++)
         if (address >= mw->min_range && address <= mw->max_range)
            break;
      if (0xFFFFFFFF == mw->min_range)
      {
         bank_writebyte(address, value);
         return;
      }
      for (before = cpu.write_handler; before != mw; before++)
         if (before->max_range >= mw->min_range && before->min_range <= mw->max_range)
            break;
      rc_mapper_at = before == mw ? mw : NULL;
   }
   mw->write_func(address, value);
}

/* a write to a device or the cartridge: it may end the slice or turn pages, so the code leaves */
__attribute__((used)) void rc_slow_write(uint32 address, uint32 value, int rem)
{
   rc_flush(rem);
   /* (the sound's channels and the pads' strobe are no line's business) */
   if (address - 0x4000 >= 0x14 && 0x4016 != address)
   {
      ppu_settle_write = 1;
      rc_settle(0);
      ppu_settle_write = 0;
   }
   /* (the PPU's, the sound's and the pads' registers turn no pages: the code goes on) */
   if (address - 0x2000 < 0x2018)
   {
      if (address < 0x4000)
         ppu_write(address, value);
      else if (address < 0x4014 || 0x4015 == address)
         apu_write(address, value);
      else
         ppu_writehigh(address, value);
      rc.rem = remaining_cycles;
      return;
   }
   {
      /* (a write that turned no page and asked nothing of the slice: the code goes on) */
      uint32 turns = nes6502_turns;
      int rem_was = remaining_cycles, pending = cpu.int_pending, burn = cpu.burn_cycles;

      if (address >= 0x8000)
         rc_mapper(address, value);
      else
         mem_writebyte(address, value);
      if (turns == nes6502_turns && rem_was == remaining_cycles && pending == cpu.int_pending
          && burn == cpu.burn_cycles)
      {
         rc.rem = remaining_cycles;
         return;
      }
   }
   rc.rem = remaining_cycles - RC_BIAS;
   rc.bias = RC_BIAS;
}

/* sta $8000 and up (a mapper's registers), from translated code without rc_slow_write's tests */
__attribute__((used)) void rc_mapper_write(uint32 address, uint32 value, int rem)
{
   uint32 turns;
   int rem_was, pending, burn;

   rc_flush(rem);
   ppu_settle_write = 1;
   rc_settle(0);
   ppu_settle_write = 0;
   turns = nes6502_turns;
   rem_was = remaining_cycles, pending = cpu.int_pending, burn = cpu.burn_cycles;
   rc_mapper(address, value);
   if (turns == nes6502_turns && rem_was == remaining_cycles && pending == cpu.int_pending
       && burn == cpu.burn_cycles)
   {
      rc.rem = remaining_cycles;
      return;
   }
   rc.rem = remaining_cycles - RC_BIAS;
   rc.bias = RC_BIAS;
}

/* a branch back over a loop that only looks: the cycles of its turns left (idle_loop). X, Y
** and the flags are the code's again from the state: a loop that counts has moved them on */
__attribute__((used)) int rc_idle(uint32 where, uint32 a, uint32 x, uint32 y)
{
   uint8 p = RC_N(rc.n) | (rc.v ? V_FLAG : 0) | (RC_Z(rc.n) ? 0 : Z_FLAG) | (rc.c ? C_FLAG : 0);
   int cycles;

   rc_flush(rc.rem);
   rc.x = x;
   rc.y = y;
   if (idle_never[where & 63] == (uint16) where)
      return 0;
   cycles = rc_settle(0);
   cycles += idle_loop(where & 0xFFFF, where >> 16, a, x, y, p, remaining_cycles);
   if (idle_passed.any)
   {
      rc.x = idle_passed.x;
      rc.y = idle_passed.y;
      rc.n = RC_NZ(idle_passed.p, 0 == (idle_passed.p & Z_FLAG));
      rc.c = idle_passed.p & C_FLAG;
      rc.v = idle_passed.p & V_FLAG;
   }
   return cycles;
}

static int rc_inside;       /* a slice that rc_slice runs: the frame's loop may be asked for the next */
static int rc_running;      /* translated code has the 6502's registers: an interrupt waits for it to be left */

/* rti in translated code that a slice runs: the flags are set and the 6502's pc returned. -1:
** the interpreter's (it has the other flags, or an interrupt waits that this lets in). */
__attribute__((used)) int rc_rti(uint32 s)
{
   uint8 p = ram[0x100 + ((s + 1) & 0xFF)];

   if (!rc_running || (0 == (p & I_FLAG) && cpu.int_pending))
      return -1;
   rc.s = (s + 3) & 0xFF;
   rc.n = RC_NZ(p, 0 == (p & Z_FLAG));
   rc.c = p & C_FLAG;
   rc.v = p & V_FLAG;
   cpu.p_reg = (cpu.p_reg & ~(B_FLAG | D_FLAG | I_FLAG)) | (p & (B_FLAG | D_FLAG | I_FLAG));
   return ram[0x100 + ((s + 2) & 0xFF)] | ram[0x100 + ((s + 3) & 0xFF)] << 8;
}

/* php, plp, sei and cli in translated code (w: the opcode, and its cycles above it). The flags
** i, d and b are cpu.p_reg's while translated code runs (RC_RUN keeps them there too). 0: on;
** 1: out after it (cli lets a waiting interrupt in); 2: out before it, for the interpreter's. */
__attribute__((used)) int rc_flag_op(uint32 w)
{
   uint8 p;

   switch (w & 0xFF)
   {
   case 0x78:
      cpu.p_reg |= I_FLAG;
      break;
   case 0x58:
      /* (the interpreter's takes the interrupt in the instruction; a slice's start takes it) */
      if (cpu.int_pending && !rc_running)
         return 2;
      cpu.p_reg &= ~I_FLAG;
      break;
   case 0x08:
      p = RC_N(rc.n) | (rc.v ? V_FLAG : 0) | R_FLAG | B_FLAG | (cpu.p_reg & (D_FLAG | I_FLAG))
          | (RC_Z(rc.n) ? 0 : Z_FLAG) | (rc.c ? C_FLAG : 0);
      ram[0x100 + rc.s] = p;
      rc.s = (rc.s - 1) & 0xFF;
      break;
   default:
      rc.s = (rc.s + 1) & 0xFF;
      p = ram[0x100 + rc.s];
      rc.n = RC_NZ(p, 0 == (p & Z_FLAG));
      rc.c = p & C_FLAG;
      rc.v = p & V_FLAG;
      cpu.p_reg = (cpu.p_reg & ~(B_FLAG | D_FLAG | I_FLAG)) | (p & (B_FLAG | D_FLAG | I_FLAG));
      break;
   }
   /* (an interrupt let in waits for the line's end, as the interpreter's does: no lines ahead) */
   if (0 == (cpu.p_reg & I_FLAG) && cpu.int_pending && ppu_lines_ahead)
   {
      rc_flush(rc.rem);
      rc_settle(0);
      rc.rem = remaining_cycles;
   }
   rc.rem -= (int) (w >> 8);
   return (0x58 == (w & 0xFF) && cpu.int_pending) ? 1 : 0;
}

/* a read of a pad's register: sixteen or more a frame, and no cycle's or line's business */
__attribute__((used)) uint32 rc_pad(uint32 address)
{
   return ppu_readhigh(address);
}

/* a write to the PPU's data register, of which a game makes thousands in a row */
extern void ppu_write_data(uint8 value);

/* (in a slice of several lines: apart, so that the write itself saves no registers) */
static __attribute__((noinline)) int rc_vdata_ahead(uint32 value, int rem)
{
   int cut;

   rc_flush(rem);
   cut = rc_settle(0);
   ppu_write_data(value);
   return cut;
}

/* (the register is no cycle's business: nothing is brought up to date for it. Returns what
** the slice is cut by) */
__attribute__((used)) int rc_vdata(uint32 value, int rem)
{
   if (ppu_lines_ahead)
      return rc_vdata_ahead(value, rem);
   ppu_write_data(value);
   return 0;
}
int rc_leave;               /* the next line has begun and the interpreter is to go on: it has an interrupt to take */

/* (rc_line for a line whose work calls something: apart, so that rc_line saves no registers) */
static __attribute__((noinline)) int rc_line_called(void)
{
   int next = ppu_line_between(cpu.total_cycles - rc_slice_from);

   if (next <= 0)
      return 0;
   rc_slice_from = cpu.total_cycles;
   remaining_cycles = next;
   /* (the cartridge counted the line and asks for an interrupt, or burns cycles) */
   if (cpu.burn_cycles || (cpu.int_pending && 0 == (cpu.p_reg & I_FLAG)))
      rc_leave = 1;
   return next;
}

/* (rc_line at the end of a slice of several lines: apart, as rc_line_called is) */
static __attribute__((noinline)) int rc_line_ahead(int rem)
{
   int next;

   rc_flush(rem);
   rc_settle(0);
   next = ppu_line_plain(cpu.total_cycles - rc_slice_from);
   if (next < 0)
      return rc_line_called();
   if (0 == next)
      return 0;
   rc_slice_from = cpu.total_cycles;
   remaining_cycles = next;
   return next;
}

/* A slice has ended in translated code. The next one's cycles, when all the frame's loop has
** to do between the two is an ordinary line's work (ppu_line_between does it); else 0, and
** the code leaves as it did. An awake line was 350 instructions round its slice, so. */
__attribute__((used)) int rc_line(int rem)
{
   int next;

   /* (an interrupt that waits is taken at a slice's start only when the 6502 lets it in: i is
   ** the interpreter's flag, which translated code leaves as it is) */
   if (!rc_inside || rem < -64 || nes6502_idle.state || cpu.burn_cycles
       || (cpu.int_pending && 0 == (cpu.p_reg & I_FLAG)))
      return 0;
   if (ppu_lines_ahead)
      return rc_line_ahead(rem);
   rc_flush(rem);
   next = ppu_line_plain(cpu.total_cycles - rc_slice_from);
   if (next < 0)
      return rc_line_called();
   if (0 == next)
      return 0;
   rc_slice_from = cpu.total_cycles;
   remaining_cycles = next;
   return next;
}

__asm__(
   ".pushsection .text\n"
   ".align 2\n"
   "rc_rt:\n"
   /* out: a0 is where the word with the 6502's pc is; what follows that word goes on from it */
   "rc_rt_out:\n"
   "   bgtz s8, 1f\n"
   "   addi sp, sp, -16\n   sw a0, 12(sp)\n      mv a0, s8\n        call rc_line\n"
   "   mv t0, a0\n          lw a0, 12(sp)\n      addi sp, sp, 16\n  beqz t0, 1f\n"
   "   mv s8, t0\n          lla t1, rc_leave\n   lw t1, 0(t1)\n   bnez t1, 1f\n   jr 4(a0)\n"
   "1: lw a0, 0(a0)\n"
   "rc_rt_out_pc:\n"
   "   sw s0, 0(s10)\n   sw s1, 4(s10)\n   sw s2, 8(s10)\n   sw s3, 12(s10)\n"
   "   sw s4, 16(s10)\n  sw s5, 20(s10)\n  sw s6, 24(s10)\n  sw s7, 28(s10)\n"
   "   sw s8, 32(s10)\n  sw a0, 36(s10)\n"
   "   lw ra, 60(sp)\n   lw s0, 56(sp)\n   lw s1, 52(sp)\n   lw s2, 48(sp)\n"
   "   lw s3, 44(sp)\n   lw s4, 40(sp)\n   lw s5, 36(sp)\n   lw s6, 32(sp)\n"
   "   lw s7, 28(sp)\n   lw s8, 24(sp)\n   lw s9, 20(sp)\n   lw s10, 16(sp)\n"
   "   lw s11, 12(sp)\n  addi sp, sp, 64\n ret\n"
   /* read: a0 an address, then its byte; t1 the cycle a page crossed took; a2 and a3 are kept */
   "rc_rt_read:\n"
   "   srli t0, a0, 11\n   bnez t0, 1f\n"
   "   add t0, s9, a0\n    lbu a0, 0(t0)\n     ret\n"
   "1: lw t0, 68(s10)\n    bltu a0, t0, 2f\n"
   "   srli t0, a0, 12\n   slli t0, t0, 2\n    lw t1, 44(s10)\n    add t0, t0, t1\n"
   "   lw t0, 0(t0)\n      slli t1, a0, 20\n   srli t1, t1, 20\n   add t0, t0, t1\n"
   "   lbu a0, 0(t0)\n     ret\n"
   "2: addi sp, sp, -16\n  sw ra, 12(sp)\n     sw a2, 8(sp)\n      sw a3, 4(sp)\n"
   "   mv a1, s8\n         mv a2, t1\n        call rc_slow_read\n"
   "   lw ra, 12(sp)\n     lw a2, 8(sp)\n      lw a3, 4(sp)\n      addi sp, sp, 16\n"
   "   lw s8, 32(s10)\n    ret\n"
   /* write: a0 an address, a1 a byte */
   "rc_rt_write:\n"
   "   srli t0, a0, 11\n   bnez t0, 1f\n"
   "   add t0, s9, a0\n    sb a1, 0(t0)\n      ret\n"
   "1: lw t0, 72(s10)\n    bltu a0, t0, 3f\n   srli t0, a0, 15\n   bnez t0, 3f\n"
   "   srli t0, a0, 12\n   slli t0, t0, 2\n    lw t1, 44(s10)\n    add t0, t0, t1\n"
   "   lw t0, 0(t0)\n      slli t1, a0, 20\n   srli t1, t1, 20\n   add t0, t0, t1\n"
   "   sb a1, 0(t0)\n      ret\n"
   "3: addi sp, sp, -16\n  sw ra, 12(sp)\n"
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
   /* the same when the pages never turn: the code is by the 6502's address */
   "rc_rt_direct:\n"
   "   blez s8, rc_rt_out_pc\n"
   "   lw t1, 76(s10)\n    slli t0, a0, 2\n    add t0, t0, t1\n    lw t1, 0(t0)\n"
   "   beqz t1, rc_rt_out_pc\n jr t1\n"
   /* rti: a0 is where the word with its pc is, for when it is the interpreter's */
   "rc_rt_rti:\n"
   "   addi sp, sp, -16\n  sw a0, 12(sp)\n     mv a0, s3\n         call rc_rti\n"
   "   bltz a0, 1f\n"
   "   lw s3, 12(s10)\n    lw s4, 16(s10)\n    lw s6, 24(s10)\n    lw s7, 28(s10)\n"
   "   addi sp, sp, 16\n   addi s8, s8, -6\n"
   "   lw t0, 76(s10)\n    bnez t0, rc_rt_direct\n j rc_rt_dispatch\n"
   "1: lw a0, 12(sp)\n     addi sp, sp, 16\n   lw a0, 0(a0)\n      j rc_rt_out_pc\n"
   /* mapper: a0 an address from 0x8000, a1 a byte */
   "rc_rt_mapper:\n"
   "   addi sp, sp, -16\n  sw ra, 12(sp)\n     mv a2, s8\n         call rc_mapper_write\n"
   "   lw ra, 12(sp)\n     addi sp, sp, 16\n   lw s8, 32(s10)\n    ret\n"
   /* flag: a1 the opcode and its cycles; at a0 the words with the pc after it and before it */
   "rc_rt_flag:\n"
   "   addi sp, sp, -16\n  sw a0, 12(sp)\n"
   "   sw s3, 12(s10)\n    sw s4, 16(s10)\n    sw s6, 24(s10)\n    sw s7, 28(s10)\n    sw s8, 32(s10)\n"
   "   mv a0, a1\n         call rc_flag_op\n"
   "   lw s3, 12(s10)\n    lw s4, 16(s10)\n    lw s6, 24(s10)\n    lw s7, 28(s10)\n    lw s8, 32(s10)\n"
   "   mv t0, a0\n         lw a0, 12(sp)\n     addi sp, sp, 16\n"
   "   beqz t0, 2f\n       addi t0, t0, -1\n   bnez t0, 1f\n"
   "   lw a0, 0(a0)\n      j rc_rt_out_pc\n"
   "1: lw a0, 4(a0)\n      j rc_rt_out_pc\n"
   "2: jr 8(a0)\n"
   /* pad: a0 the address of a pad's register, then its byte */
   "rc_rt_pad:\n"
   "   addi sp, sp, -16\n  sw ra, 12(sp)\n     call rc_pad\n"
   "   lw ra, 12(sp)\n     addi sp, sp, 16\n   ret\n"
   /* vdata: a0 a byte for the PPU's data register */
   "rc_rt_vdata:\n"
   "   addi sp, sp, -16\n  sw ra, 12(sp)\n     mv a1, s8\n         call rc_vdata\n"
   "   lw ra, 12(sp)\n     addi sp, sp, 16\n   sub s8, s8, a0\n    ret\n"
   /* idle: after a taken branch back; the word after the call is the loop's top and end */
   "rc_rt_idle:\n"
   "   blez s8, 9f\n"
   "   addi sp, sp, -16\n  sw ra, 12(sp)\n"
   "   sw s4, 16(s10)\n    sw s5, 20(s10)\n    sw s6, 24(s10)\n    sw s7, 28(s10)\n"
   "   sw s8, 32(s10)\n"
   "   lw a0, 0(ra)\n      mv a1, s0\n         mv a2, s1\n         mv a3, s2\n"
   "   call rc_idle\n"
   "   lw ra, 12(sp)\n     addi sp, sp, 16\n   sub s8, s8, a0\n"
   "   lw s1, 4(s10)\n     lw s2, 8(s10)\n     lw s4, 16(s10)\n    lw s6, 24(s10)\n    lw s7, 28(s10)\n"
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
       RA_ = 8 /* A */, RX = 9, RY = 18, RS = 19, RN = 20, RM1 = 21, RC = 22, RV = 23,
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
#define  e_slt(rd, a, b)         E_R(0, b, a, 2, rd)
#define  e_addi(rd, a, imm)      E_I(0x13, 0, rd, a, imm)
#define  e_xori(rd, a, imm)      E_I(0x13, 4, rd, a, imm)
#define  e_andi(rd, a, imm)      E_I(0x13, 7, rd, a, imm)
#define  e_slli(rd, a, n)        E_I(0x13, 1, rd, a, n)
#define  e_srli(rd, a, n)        E_I(0x13, 5, rd, a, n)
#define  e_mv(rd, a)             e_addi(rd, a, 0)
#define  e_lbu(rd, base, off)    E_I(0x03, 4, rd, base, off)
#define  e_lw(rd, base, off)     E_I(0x03, 2, rd, base, off)
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
/* to the code of the 6502's address in a0, wherever that is */
#define  e_dispatch()            e_jalr(ZERO, RT, RT_OFF(rc_fixed ? rc_rt_direct : rc_rt_dispatch))

/* leave with the 6502 at pc */
static void e_out(uint32 pc)
{
   e_jalr(A0, RT, RT_OFF(rc_rt_out));
   E(pc);
}

/* A run's unchecked copy (rc_run_copy): its instructions' cycles are counted here, not by the
** code, until something must have them (rc_owed), and N and Z are set by its last setter only. */
static int rc_fast, rc_pending, rc_nz_live;
/* in a copy: whether an instruction's C and V are read before they are set again (rc_flags),
** and what C is known to be before it (after clc or sec), or -1 */
static int rc_c_live, rc_v_live, rc_c_known = -1;
static int rc_extra;   /* cycles of a run's copy that the jump ending it takes with its own */
static uint8 *rc_lead; /* by byte of the program: 1 a jump comes here, 2 looked at (rc_leaders) */

static void rc_owed(void)
{
   if (rc_fast && rc_pending)
      e_addi(REM, REM, -rc_pending);
   rc_pending = 0;
}

/* an instruction's end: its cycles, and out if none are left */
static void e_tail(int cycles, uint32 next)
{
   if (rc_fast)
   {
      rc_pending += cycles;
      return;
   }
   e_addi(REM, REM, -cycles);
   e_bgtz(REM, 12);
   e_out(next);
}

#define  rc_nz_wanted()          (!rc_fast || rc_nz_live)
#define  rc_c_wanted()           (!rc_fast || rc_c_live)
#define  rc_v_wanted()           (!rc_fast || rc_v_live)
#define  e_nz(reg)               (rc_nz_wanted() ? (void) e_mv(RN, reg) : (void) 0)   /* (a result is 0 to 255) */

/* ---- the 6502's instructions ---- */

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_INX, M_INY, M_REL };
enum { K_NONE, K_INTERP1, K_LDA, K_LDX, K_LDY, K_STA, K_STX, K_STY, K_ADC, K_SBC, K_AND, K_ORA,
       K_EOR, K_CMP, K_CPX, K_CPY, K_BIT, K_INC, K_DEC, K_ASL, K_LSR, K_ROL, K_ROR, K_INX, K_INY,
       K_DEX, K_DEY, K_TAX, K_TAY, K_TXA, K_TYA, K_TSX, K_TXS, K_PHA, K_PLA, K_CLC, K_SEC, K_CLV,
       K_NOP, K_BRANCH, K_JMP, K_JSR, K_RTS, K_JMPI, K_RTI, K_FLAG };

typedef struct { uint8 kind, mode, cycles; } rc_op_t;

/* (not here, so the interpreter's: brk, cld, sed, all undocumented; and
** jmp () through a pointer that is not in RAM; rti is a routine's, in a slice of rc_slice's) */
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
   [0x4C] = { K_JMP, M_ABS, 3 }, [0x6C] = { K_JMPI, M_ABS, 5 }, [0x20] = { K_JSR, M_ABS, 6 }, [0x60] = { K_RTS, M_IMP, 6 },
   [0x40] = { K_RTI, M_IMP, 6 },
   [0x08] = { K_FLAG, M_IMP, 3 }, [0x28] = { K_FLAG, M_IMP, 4 }, [0x58] = { K_FLAG, M_IMP, 2 }, [0x78] = { K_FLAG, M_IMP, 2 },
   [0xD8] = { K_INTERP1 }, [0xF8] = { K_INTERP1 },
#undef   ALU
#undef   SHIFT
#undef   STEP
};

static const uint8 rc_length[12] = { 1, 1, 2, 2, 2, 2, 3, 3, 3, 2, 2, 2 };

/* ---- translation ---- */

#define  RC_WORK  4096

typedef struct { uint32 *site; uint32 pc; } rc_fix_t;

/* (mapped with the tables, not the program's own memory: only the pages that are used are there) */
static uint32 *rc_work, *rc_stack;
static rc_fix_t *rc_fix;
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

#define  rc_is_pad(mode, arg)    (M_ABS == (mode) && (0x4016 == (arg) || 0x4017 == (arg)))

/* whether a mode's operand is read through the routine, which may come to a device */
static int rc_read_called(int mode, uint32 arg)
{
   if (M_IMM == mode || M_IMP == mode || M_ACC == mode || rc_in_ram(mode, arg) || rc_is_pad(mode, arg))
      return 0;
   if (arg >= rc.paged_read && arg < 0x8000 && (M_ABS == mode || ((M_ABX == mode || M_ABY == mode) && (arg & 0xFFF) + 0xFF < 0x1000)))
      return 0;   /* (the cartridge's own memory at 0x6000, no device's: through the pages) */
   return !((M_ABS == mode && arg >= 0x8000)
            || ((M_ABX == mode || M_ABY == mode) && arg >= 0x8000 && (arg & 0xFFF) + 0xFF < 0x1000));
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
   else if (rc_is_pad(mode, arg))
   {
      e_li(A0, arg);
      e_call(rc_rt_pad);
      if (to != A0)
         e_mv(to, A0);
   }
   else if (!rc_read_called(mode, arg))
   {
      /* the cartridge, and all of it in one page: through the pages as they are now, in place */
      uint32 in_page = arg & 0xFFF;

      if (penalty && M_ABS != mode)
         e_cross(M_ABX == mode ? RX : RY, arg);
      e_lw(T0, 26 /* s10 */, 44);
      e_lw(T0, T0, 4 * (arg >> 12));
      if (M_ABS != mode)
         e_add(T0, T0, M_ABX == mode ? RX : RY);
      if (in_page >= 0x800)
      {
         e_li(T1, in_page);
         e_add(T0, T0, T1);
         in_page = 0;
      }
      e_lbu(to, T0, in_page);
   }
   else
   {
      e_address(mode, arg, penalty);
      /* (t1 for the routine: the cycle a read across a page took, which e_address leaves there) */
      if (!penalty || (M_ABX != mode && M_ABY != mode && M_INY != mode))
         e_mv(T1, ZERO);
      rc_owed();
      e_call(rc_rt_read);
      if (to != A0)
         e_mv(to, A0);
   }
}

/* the place of a byte of the cartridge's memory at 0x6000 through the pages, in t0 (t1 used) */
static int e_paged_place(uint32 arg)
{
   uint32 in_page = arg & 0xFFF;

   e_lw(T0, 26 /* s10 */, 44);
   e_lw(T0, T0, 4 * (arg >> 12));
   if (in_page >= 0x800)
   {
      e_li(T1, in_page);
      e_add(T0, T0, T1);
      in_page = 0;
   }
   return (int) in_page;
}

#define  rc_paged_rw(mode, arg)  (M_ABS == (mode) && (arg) >= rc.paged_write && (arg) >= rc.paged_read && (arg) < 0x8000)

static void e_store(int mode, uint32 arg, int from)
{
   int base, offset;

   if (rc_in_ram(mode, arg))
   {
      base = e_ram_place(mode, arg, &offset);
      e_sb(from, base, offset);
   }
   else if (rc_paged_rw(mode, arg))
      e_sb(from, T0, e_paged_place(arg));
   else if ((M_ABX == mode || M_ABY == mode) && arg >= rc.paged_write && arg < 0x8000 && (arg & 0xFFF) + 0xFF < 0x1000)
   {
      offset = e_paged_place(arg);
      e_add(T0, T0, M_ABX == mode ? RX : RY);
      e_sb(from, T0, offset);
   }
   else if (M_ABS == mode && 0x2007 == (arg & 0xE007))
   {
      e_mv(A0, from);
      rc_owed();
      e_call(rc_rt_vdata);
   }
   else if (M_ABS == mode && arg >= 0x8000)
   {
      e_li(A0, arg);
      e_mv(A1, from);
      rc_owed();
      e_call(rc_rt_mapper);
   }
   else
   {
      e_address(mode, arg, 0);
      e_mv(A1, from);
      rc_owed();
      e_call(rc_rt_write);
   }
}

/* asl, lsr, rol, ror, inc, dec of a register (t1 is used) */
static void e_modify(int kind, int reg)
{
   switch (kind)
   {
   case K_ASL:
      if (rc_c_wanted())
         e_srli(RC, reg, 7);
      e_slli(reg, reg, 1);
      e_andi(reg, reg, 0xFF);
      break;
   case K_LSR:
      if (rc_c_wanted())
         e_andi(RC, reg, 1);
      e_srli(reg, reg, 1);
      break;
   case K_ROL:
      if (rc_c_wanted())
         e_srli(T1, reg, 7);
      e_slli(reg, reg, 1);
      e_or(reg, reg, RC);
      e_andi(reg, reg, 0xFF);
      if (rc_c_wanted())
         e_mv(RC, T1);
      break;
   case K_ROR:
      e_slli(T1, RC, 7);
      if (rc_c_wanted())
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

/* the flags of a comparison whose difference is in t1 (C: it is not below zero) */
static void e_compared(void)
{
   if (rc_c_wanted())
      e_slt(RC, RM1, T1);
   if (rc_nz_wanted())
      e_andi(RN, T1, 0xFF);
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

/* whether a loop is one idle_loop could pass over: loads, compares, tests and branches only;
** or one that counts, with dex, dey, inx or iny as its last before the bne that closes it,
** no other branch, and nothing else of X or Y */
static int rc_idle_possible(uint32 top, uint32 end)
{
   uint32 pc = top;
   int counts = 0, branches = 0, indexes = 0;

   while (pc < end)
   {
      uint8 op = bank_readbyte(pc);
      const rc_op_t *info = &rc_ops[op];

      if (K_LDX == info->kind || K_LDY == info->kind || K_CPX == info->kind || K_CPY == info->kind)
         indexes++;
      if (K_BRANCH == info->kind || K_JMP == info->kind)
         branches++;
      if (K_INX == info->kind || K_INY == info->kind || K_DEX == info->kind || K_DEY == info->kind)
      {
         if (pc + 3 != end || 0xD0 != bank_readbyte(pc + 1))
            return 0;
         counts++;
         pc += 1;
      }
      else if (K_BRANCH == info->kind)
         pc += 2;
      else if (K_JMP == info->kind && pc + 3 == end)
         pc += 3;   /* (the jump that closes it: a game that waits in jmp to itself) */
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
            if (0x2002 == address)
               indexes++;   /* (a loop that counts does not read the status) */
         }
         pc += rc_length[info->mode];
      }
      else
         return 0;
   }
   if (counts && (1 != branches || indexes))
      return 0;
   return pc == end;
}

/* A jsr to code not translated yet leaves, and its routine is translated when it is first
** called (most of a cartridge is not, in its first frames): the jsr is then made a jump. */
typedef struct { uint32 *site; uint32 where; uint32 page; } rc_later_t;

static rc_later_t *rc_later;
static int rc_laters, rc_later_room, rc_lazy;

static int rc_later_add(uint32 *site, uint32 where, uint32 page)
{
   /* (room for RC_WORK, mapped with the tables: no malloc, which another core may be in) */
   if (rc_laters == rc_later_room)
      return 0;
   rc_later[rc_laters].site = site;
   rc_later[rc_laters].where = where;
   rc_later[rc_laters++].page = page;
   return 1;
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
      if (rc_lazy && rc_later_add(rc_p, where, target >> 12))
      {
         e_out(target);
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
   e_dispatch();
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
   int reg, base, offset, extra = rc_extra;

   rc_extra = 0;
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
      if (M_IMM == mode && !rc_v_wanted())
         e_addi(T1, RA_, arg & 0xFF);
      else
      {
         e_load(mode, arg, 1, A0);
         e_add(T1, RA_, A0);
      }
      if (0 != rc_c_known)
         e_add(T1, T1, RC);
      if (rc_v_wanted())
      {
         e_xor(T2, RA_, A0);
         e_xori(T2, T2, -1);
         e_xor(T0, RA_, T1);
         e_and(T2, T2, T0);
         e_andi(RV, T2, 0x80);
      }
      if (rc_c_wanted())
         e_srli(RC, T1, 8);
      e_andi(RA_, T1, 0xFF);
      e_nz(RA_);
      break;
   case K_SBC:
      if (M_IMM == mode && !rc_v_wanted())
         e_addi(T1, RA_, -(int) (arg & 0xFF));
      else
      {
         e_load(mode, arg, 1, A0);
         e_sub(T1, RA_, A0);
      }
      if (1 != rc_c_known)
      {
         e_xori(T0, RC, 1);
         e_sub(T1, T1, T0);
      }
      if (rc_v_wanted())
      {
         e_xor(T2, RA_, A0);
         e_xor(T0, RA_, T1);
         e_and(T2, T2, T0);
         e_andi(RV, T2, 0x80);
      }
      if (rc_c_wanted())
         e_slt(RC, RM1, T1);
      e_andi(RA_, T1, 0xFF);
      e_nz(RA_);
      break;
   case K_AND: case K_ORA: case K_EOR:
      if (M_IMM == mode)
      {
         E_I(0x13, (K_AND == kind) ? 7 : (K_ORA == kind) ? 6 : 4, RA_, RA_, arg & 0xFF);
         e_nz(RA_);
         break;
      }
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
      reg = (K_CMP == kind) ? RA_ : (K_CPX == kind) ? RX : RY;
      if (M_IMM == mode)
         e_addi(T1, reg, -(int) (arg & 0xFF));
      else
      {
         e_load(mode, arg, 1, A0);
         e_sub(T1, reg, A0);
      }
      e_compared();
      break;
   case K_BIT:
      e_load(mode, arg, 0, A0);
      if (rc_v_wanted())
         e_andi(RV, A0, 0x40);
      if (rc_nz_wanted())
      {
         e_andi(T0, A0, 0x80);
         e_slli(T0, T0, 1);
         e_and(RN, A0, RA_);
         e_or(RN, RN, T0);
      }
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
      else if (rc_paged_rw(mode, arg))
      {
         offset = e_paged_place(arg);
         e_mv(A3, T0);
         e_lbu(A0, A3, offset);
         e_modify(kind, A0);
         e_sb(A0, A3, offset);
      }
      else
      {
         e_address(mode, arg, 0);
         e_mv(A2, A0);
         e_mv(T1, ZERO);
         rc_owed();
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
   case K_CLC: if (rc_c_wanted()) e_mv(RC, ZERO); break;
   case K_SEC: if (rc_c_wanted()) e_addi(RC, ZERO, 1); break;
   case K_CLV: if (rc_v_wanted()) e_mv(RV, ZERO); break;
   case K_NOP: break;

   case K_BRANCH:
   {
      static const uint8 flag_reg[4] = { T0, RV, RC, T0 };
      int which = op >> 6, when_set = (op >> 5) & 1;
      int32 back = (int8) arg;
      uint32 target = (next + back) & 0xFFFF, *skip;
      int taken = (((int) back + (int) (next & 0xFF)) & 0x100) ? 4 : 3;

      if (0 == which)
         e_andi(T0, RN, 0x180);
      else if (3 == which)
         e_andi(T0, RN, 0xFF);   /* (zero when the flag is set) */
      skip = rc_p;
      E(0);
      e_addi(REM, REM, -taken - extra);
      if (back <= -3 && back >= -16 && rc_idle_possible(target, next))
      {
         e_call(rc_rt_idle);
         E(target | next << 16);
      }
      e_leave_to(pc, target);
      *skip = rc_branch_word((when_set != (3 == which)) ? 0 : 1, flag_reg[which], ZERO,
                             (int32) ((uint32) rc_p - (uint32) skip));
      e_tail(2 + extra, next);
      return;
   }
   case K_JMP:
      e_addi(REM, REM, -3 - extra);
      /* a jump a few bytes back, or to itself: a loop a game may wait in, as a branch back is */
      if (arg <= pc && pc - arg <= 16 && rc_idle_possible(arg, pc + 3))
      {
         e_call(rc_rt_idle);
         E(arg | (pc + 3) << 16);
      }
      e_leave_to(pc, arg);
      return;
   case K_JMPI:
      /* through a pointer in RAM (rc_translate leaves the others): its high byte from the
      ** same page, the 6502's own mistake; then wherever that is, as rts goes */
      base = e_ram_place(M_ABS, arg, &offset);
      e_lbu(A0, base, offset);
      base = e_ram_place(M_ABS, (arg & 0xFF00) | ((arg + 1) & 0xFF), &offset);
      e_lbu(T1, base, offset);
      e_slli(T1, T1, 8);
      e_or(A0, A0, T1);
      e_addi(REM, REM, -5 - extra);
      e_dispatch();
      return;
   case K_JSR:
      e_li(T1, (pc + 2) >> 8);
      e_push(T1);
      e_li(T1, (pc + 2) & 0xFF);
      e_push(T1);
      e_addi(REM, REM, -6 - extra);
      rc_lazy = !rc_eager;
      e_leave_to(pc, arg);
      rc_lazy = 0;
      return;
   case K_RTI:
      e_jalr(A0, RT, RT_OFF(rc_rt_rti));
      E(pc);
      return;
   case K_FLAG:
      rc_owed();
      e_li(A1, op | (info->cycles + extra) << 8);
      e_jalr(A0, RT, RT_OFF(rc_rt_flag));
      E(next);
      E(pc);
      e_bgtz(REM, 12);
      e_out(next);
      return;
   case K_RTS:
      e_pull(A0);
      e_pull(T1);
      e_slli(T1, T1, 8);
      e_or(A0, A0, T1);
      e_addi(A0, A0, 1);
      e_addi(REM, REM, -6 - extra);
      e_dispatch();
      return;
   }
   e_tail(info->cycles, next);
}

/* Whether an instruction may be in a run's unchecked copy: one that neither jumps nor can leave
** (a write that is not RAM's may), and what it takes at most; *nz: it sets N and Z. */
static int rc_plain(uint32 pc, int *most, int *nz)
{
   uint8 op = bank_readbyte(pc);
   const rc_op_t *info = &rc_ops[op];
   uint32 arg = bank_readbyte(pc + 1);
   int kind = info->kind, mode = info->mode;

   if (3 == rc_length[mode])
      arg |= bank_readbyte(pc + 2) << 8;
   if (kind < K_LDA || kind > K_NOP)
      return 0;
   if ((K_STA == kind || K_STX == kind || K_STY == kind || (kind >= K_INC && kind <= K_ROR && M_ACC != mode)) && !rc_in_ram(mode, arg))
      return 0;
   /* (a read that may be a device's: the slice may be cut there, so the cycles are tested after it) */
   if (rc_read_called(mode, arg))
      return 0;
   *nz = !(K_STA == kind || K_STX == kind || K_STY == kind || K_TXS == kind || K_PHA == kind || K_CLC == kind
           || K_SEC == kind || K_CLV == kind || K_NOP == kind);
   *most = info->cycles + 1;   /* (and the cycle a read across a page takes) */
   return rc_length[mode];
}

#define  RC_RUN_LEAST   4     /* with nothing after it to take its cycles */
#define  RC_RUN_MOST    24

#define  rc_ends_run(kind)   (K_BRANCH == (kind) || K_JMP == (kind) || K_JSR == (kind) || K_RTS == (kind))

/* Marks where the jumps of the code reached from pc come to: a run begins at each, so that a
** loop's body is a run from its top. */
static void rc_leaders(uint32 start)
{
   uint32 *stack = rc_stack;
   int n = 0;

   stack[n++] = start;
   while (n)
   {
      uint32 pc = stack[--n];

      for (;;)
      {
         uint32 where = rc_where(pc), target = 0xFFFFFFFF;
         const rc_op_t *info;
         uint8 op;

         if (where >= rc.size || (rc_lead[where] & 2) || rc.table[where])
            break;
         rc_lead[where] |= 2;
         op = bank_readbyte(pc);
         info = &rc_ops[op];
         if (K_NONE == info->kind)
            break;
         if (K_BRANCH == info->kind)
            target = (pc + 2 + (int8) bank_readbyte(pc + 1)) & 0xFFFF;
         else if (K_JMP == info->kind || K_JSR == info->kind)
            target = bank_readbyte(pc + 1) | bank_readbyte(pc + 2) << 8;
         if (rc_where(target) < rc.size)
         {
            rc_lead[rc_where(target)] |= 1;
            /* (a jsr's routine is looked at when it is translated) */
            if (n < RC_WORK && (K_JSR != info->kind || rc_eager))
               stack[n++] = target;
         }
         if (K_JMP == info->kind || K_RTS == info->kind || K_JMPI == info->kind || K_RTI == info->kind)
            break;
         pc += (K_INTERP1 == info->kind) ? 1 : rc_length[info->mode];
      }
   }
}

/* How many instructions from pc are a run: plain ones, none translated yet, all in the program. */
static int rc_run_length(uint32 pc, int *most, int *last_nz)
{
   int n = 0, cycles, nz, length;

   *most = 0;
   *last_nz = -1;
   while (n < RC_RUN_MOST)
   {
      uint32 where = rc_where(pc);

      if (where >= rc.size || where + 3 > rc.size || rc.table[where] || (n && (rc_lead[where] & 1))
          || 0 == (length = rc_plain(pc, &cycles, &nz)))
         break;
      if (nz)
         *last_nz = n;
      *most += cycles;
      pc += length;
      n++;
   }
   return n;
}

static void rc_instruction(uint32 pc, uint8 op, uint32 arg);

#define  rc_sets_c(kind)   (K_ADC == (kind) || K_SBC == (kind) || K_CMP == (kind) || K_CPX == (kind) || K_CPY == (kind) \
                            || ((kind) >= K_ASL && (kind) <= K_ROR) || K_CLC == (kind) || K_SEC == (kind))

/* For each of a run's n instructions: whether the C and the V it sets are read before they are
** set again (all are at the run's end), and what C is known to be as it begins, or -1. */
static void rc_flags(uint32 pc, int n, uint8 *c_live, uint8 *v_live, int8 *c_known)
{
   uint8 kinds[RC_RUN_MOST];
   int i, known = -1, c = 1, v = 1;

   for (i = 0; i < n; i++)
   {
      const rc_op_t *info = &rc_ops[bank_readbyte(pc)];

      kinds[i] = info->kind;
      c_known[i] = known;
      if (K_CLC == info->kind || K_SEC == info->kind)
         known = (K_SEC == info->kind);
      else if (rc_sets_c(info->kind))
         known = -1;
      pc += rc_length[info->mode];
   }
   for (i = n - 1; i >= 0; i--)
   {
      int kind = kinds[i];

      c_live[i] = c;
      v_live[i] = v;
      if (rc_sets_c(kind))
         c = 0;
      if ((K_ADC == kind && 0 != c_known[i]) || (K_SBC == kind && 1 != c_known[i]) || K_ROL == kind || K_ROR == kind)
         c = 1;
      if (K_ADC == kind || K_SBC == kind || K_BIT == kind || K_CLV == kind)
         v = 0;
   }
}

/* inx, iny, dex, dey, asl a, lsr a: several of one in a row are done at once in a run's copy */
#define  rc_repeats(op)    (0xE8 == (op) || 0xC8 == (op) || 0xCA == (op) || 0x88 == (op) || 0x0A == (op) || 0x4A == (op))

static void e_repeated(uint8 op, int times)
{
   int reg = (0xE8 == op || 0xCA == op) ? RX : (0xC8 == op || 0x88 == op) ? RY : RA_;

   if (0x4A == op)
   {
      if (rc_c_wanted())
      {
         e_srli(RC, reg, times - 1);
         e_andi(RC, RC, 1);
      }
      e_srli(reg, reg, times);
   }
   else
   {
      if (0x0A != op)
         e_addi(reg, reg, (0xE8 == op || 0xC8 == op) ? times : -times);
      else
      {
         if (rc_c_wanted())
         {
            e_srli(RC, reg, 8 - times);
            e_andi(RC, RC, 1);
         }
         e_slli(reg, reg, times);
      }
      e_andi(reg, reg, 0xFF);
   }
   e_nz(reg);
   rc_pending += 2 * times;
}

/* A run's unchecked copy, before its instructions one by one: taken when the cycles left are
** more than the run can take, so that none of it could have been the slice's last. Returns
** the jump at its end, to be pointed past the run's own code. */
static uint32 *rc_run_copy(uint32 pc, int n, int most, int last_nz)
{
   uint32 *guard, *jump;
   uint8 c_live[RC_RUN_MOST], v_live[RC_RUN_MOST];
   int8 c_known[RC_RUN_MOST];
   int i, same;

   rc_flags(pc, n, c_live, v_live, c_known);
   e_addi(T0, REM, -most);
   guard = rc_p;
   E(0);
   rc_fast = 1;
   rc_pending = 0;
   for (i = 0; i < n; i += same)
   {
      uint8 op = bank_readbyte(pc);
      const rc_op_t *info = &rc_ops[op];
      uint32 arg = bank_readbyte(pc + 1);

      if (3 == rc_length[info->mode])
         arg |= bank_readbyte(pc + 2) << 8;
      for (same = 1; rc_repeats(op) && same < 7 && i + same < n && bank_readbyte(pc + same) == op; same++)
         continue;
      /* (the last of them is the one whose flags may be read) */
      rc_nz_live = (last_nz >= i && last_nz < i + same);
      rc_c_live = c_live[i + same - 1];
      rc_v_live = v_live[i];
      rc_c_known = c_known[i];
      if (same > 1)
         e_repeated(op, same);
      else
         rc_instruction(pc, op, arg);
      pc += same * rc_length[info->mode];
   }
   rc_c_known = -1;
   {
      /* the jump that ends the run, in the copy too: the run's cycles go with its own */
      uint8 op = bank_readbyte(pc);
      const rc_op_t *info = &rc_ops[op];
      uint32 arg = bank_readbyte(pc + 1);

      if (rc_ends_run(info->kind) && rc_where(pc) + 3 <= rc.size)
      {
         if (3 == rc_length[info->mode])
            arg |= bank_readbyte(pc + 2) << 8;
         rc_fast = 0;
         rc_extra = rc_pending;
         rc_pending = 0;
         rc_instruction(pc, op, arg);
         if (K_BRANCH == info->kind)
            e_jump(pc, pc + 2);
         jump = NULL;
      }
      else
      {
         rc_owed();
         rc_fast = 0;
         jump = rc_p;
         E(0);
      }
   }
   /* (blez t0: not enough left, the instructions one by one) */
   *guard = rc_branch_word(5, ZERO, T0, (int32) ((uint32) rc_p - (uint32) guard));
   return jump;
}

/* Translates what is reached from the 6502's pc by jumps that are straight ones. */
static void rc_translate(uint32 start)
{
   int i;

   rc_works = rc_fixes = 0;
   rc_work[rc_works++] = start;
   rc_runs++;
   if (!rc_plain_runs_off)
      rc_leaders(start);

   while (rc_works)
   {
      uint32 pc = rc_work[--rc_works];
      int falls = 0, run_left = 0;
      uint32 *run_jump = NULL;

      for (;;)
      {
         uint32 where = rc_where(pc), arg, code = (uint32) rc_p;
         const rc_op_t *info;
         uint8 op;

         /* (room: the longest instruction, a run's copy, and what the jumps still to mend may need) */
         if (where >= rc.size || rc_p + 64 + 24 * RC_RUN_MOST + 2 * rc_fixes > rc_end)
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
         if (0 == run_left && !rc_plain_runs_off)
         {
            int most, last_nz, n = rc_run_length(pc, &most, &last_nz);
            uint32 after = pc;
            int i;

            for (i = 0; i < n; i++)
               after += rc_length[rc_ops[bank_readbyte(after)].mode];
            if (n >= RC_RUN_LEAST || (n >= 2 && rc_where(after) + 3 <= rc.size && rc_ends_run(rc_ops[bank_readbyte(after)].kind)))
            {
               /* (the run's first instruction is entered at the copy's test) */
               run_jump = rc_run_copy(pc, n, most, last_nz);
               run_left = n;
            }
         }
         op = bank_readbyte(pc);
         info = &rc_ops[op];
         rc.at[where] = pc >> 12;
         if (K_NONE == info->kind || K_INTERP1 == info->kind
             || (K_JMPI == info->kind && !rc_in_ram(M_ABS, bank_readbyte(pc + 1) | bank_readbyte(pc + 2) << 8)))
         {
            /* the interpreter's: fallen into, the code leaves; never entered */
            rc.table[where] = code | 1;
            e_out(pc);
            if (K_INTERP1 != info->kind)
               break;
            pc += 1;
            falls = 0;
            continue;
         }
         arg = bank_readbyte(pc + 1);
         if (3 == rc_length[info->mode])
            arg |= bank_readbyte(pc + 2) << 8;
         /* (rti, php, plp, sei and cli are come to from translated code only: found at a slice's
         ** start, or left before, they are the interpreter's) */
         rc.table[where] = (K_RTI == info->kind || K_FLAG == info->kind) ? (code | 1) : code;
         if (rc.direct && K_RTI != info->kind && K_FLAG != info->kind)
            rc.direct[pc] = code;
         rc_instruction(pc, op, arg);
         rc_count++;
         if (run_left && 0 == --run_left && run_jump)
            *run_jump = rc_jal_word(ZERO, (int32) ((uint32) rc_p - (uint32) run_jump));
         if (K_JMP == info->kind || K_RTS == info->kind || K_JMPI == info->kind || K_RTI == info->kind)
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

   /* the jsrs whose routines are there now */
   {
      int kept = 0;

      for (i = 0; i < rc_laters; i++)
      {
         uint32 code = rc.table[rc_later[i].where] & ~1u;

         if (code && rc.at[rc_later[i].where] == rc_later[i].page)
            *rc_later[i].site = rc_jal_word(ZERO, (int32) (code - (uint32) rc_later[i].site));
         else
            rc_later[kept++] = rc_later[i];
      }
      rc_laters = kept;
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
            rc.n = RC_NZ(n_flag, z_flag); rc.c = c_flag; rc.v = v_flag; \
            cpu.p_reg = (cpu.p_reg & ~(B_FLAG | D_FLAG | I_FLAG)) | (b_flag ? B_FLAG : 0) | (d_flag ? D_FLAG : 0) | (i_flag ? I_FLAG : 0); \
            rc.rem = remaining_cycles; \
            rc.ram = ram; \
            rc_entries++; \
            rc_enter(rc.table[rc_at], &rc, rc_rt); \
            A = rc.a; X = rc.x; Y = rc.y; S = rc.s; \
            n_flag = RC_N(rc.n); z_flag = RC_Z(rc.n); c_flag = rc.c; v_flag = rc.v; \
            b_flag = cpu.p_reg & B_FLAG; d_flag = cpu.p_reg & D_FLAG; i_flag = cpu.p_reg & I_FLAG; \
            PC = rc.pc; \
            rc_flush(rc.rem + rc.bias); \
            rc.bias = 0; \
            ran = 1; \
         } \
      } \
   } \
}

/* A slice that begins in translated code, run without the interpreter's function, which is
** a large one to come into and out of. True: the slice is done. Else the interpreter goes on
** from the registers as they are, with remaining_cycles; either way *elapsed is what ran. */
static int rc_slice(int timeslice_cycles, int *elapsed)
{
   int done = 0;
   uint32 p = cpu.p_reg, pc = cpu.pc_reg;

   rc_slice_from = cpu.total_cycles;

   *elapsed = 0;
   remaining_cycles = timeslice_cycles;
   if (NULL == rc.table || timeslice_cycles <= 0 || (cpu.int_pending && 0 == (p & I_FLAG)))
      return 0;
   /* cycles to burn, as the interpreter's slice begins with them: after an interrupt, or a copy
   ** of the sprites, which is four lines and a half. A line all burnt goes on into the next
   ** from here, as one that ends in translated code does. */
   while (cpu.burn_cycles)
   {
      int burn_for = MIN(remaining_cycles, cpu.burn_cycles), next;

      remaining_cycles -= burn_for;
      cpu.total_cycles += burn_for;
      cpu.burn_cycles -= burn_for;
      if (remaining_cycles > 0)
         break;
      rc_settle(0);
      if (rc_plain_runs_off || nes6502_idle.state)
         next = 0;
      else if ((next = ppu_line_plain(cpu.total_cycles - rc_slice_from)) < 0)
         next = ppu_line_between(cpu.total_cycles - rc_slice_from);
      if (next <= 0)
      {
         *elapsed = cpu.total_cycles - rc_slice_from;
         return 1;
      }
      rc_slice_from = cpu.total_cycles;
      remaining_cycles = next;
      /* (the cartridge counted the line and asks for an interrupt: the interpreter's, at this slice's start) */
      if (cpu.int_pending && 0 == (p & I_FLAG))
      {
         rc_settle(0);
         return 0;
      }
   }
   rc.a = cpu.a_reg; rc.x = cpu.x_reg; rc.y = cpu.y_reg; rc.s = cpu.s_reg;
   rc.n = RC_NZ(p, 0 == (p & Z_FLAG)); rc.c = p & C_FLAG; rc.v = p & V_FLAG;
   rc.ram = ram;
   for (;;)
   {
      uint32 at = rc_where(pc);

      if (at >= rc.size)
         break;
      if (0 == rc.table[at] && !rc_full)
         rc_translate(pc);
      if ((rc.table[at] & 1) || 0 == rc.table[at] || rc.at[at] != (pc >> 12))
         break;
      rc.rem = remaining_cycles;
      rc_entries++;
      rc_inside = !rc_plain_runs_off;
      rc_running = 1;
      rc_enter(rc.table[at], &rc, rc_rt);
      rc_inside = rc_running = 0;
      pc = rc.pc;
      rc_flush(rc.rem + rc.bias);
      rc.bias = 0;
      rc_settle(0);
      if (rc_leave)
      {
         rc_leave = 0;
         break;
      }
      if (remaining_cycles <= 0)
      {
         done = 1;
         break;
      }
   }
   cpu.pc_reg = pc;
   cpu.a_reg = rc.a; cpu.x_reg = rc.x; cpu.y_reg = rc.y; cpu.s_reg = rc.s;
   cpu.p_reg = (cpu.p_reg & (B_FLAG | D_FLAG | I_FLAG)) | R_FLAG | RC_N(rc.n) | (rc.v ? V_FLAG : 0)
               | (RC_Z(rc.n) ? 0 : Z_FLAG) | (rc.c ? C_FLAG : 0);
   /* (as the interpreter's slice ends: one that found an idle loop and ends inside it sleeps) */
   if (done && 1 == idle.state)
   {
      int at;

      idle.state = 0;
      for (at = 0; at < idle.count && !idle.waking && 2 != nes6502_plain; at++)
      {
         if (idle.pc[at] == pc)
         {
            idle.at = idle.was_at = at;
            idle.state = 2;
            idle_count_from(at, cpu.x_reg, cpu.y_reg);
            break;
         }
      }
   }
   *elapsed = cpu.total_cycles - rc_slice_from;
   return done;
}

/* Called once the cartridge is in: its program, and whether its pages ever turn. */
void nes6502_recompile(uint8 *prg, int bytes, int fixed)
{
   rc_code = mmap(NULL, RC_CODE_BYTES, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (MAP_FAILED == (void *) rc_code)
      return;
   rc_p = rc_code;
   rc_end = rc_code + RC_CODE_BYTES / 4;
   {
      /* the tables by byte of the program and the translator's lists: mapped, so zero and untouched */
      uint32 all = (uint32) bytes * 6 + RC_WORK * (2 * sizeof(uint32) + sizeof(rc_fix_t) + sizeof(rc_later_t));
      uint8 *tables = mmap(NULL, all, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

      if (MAP_FAILED == (void *) tables)
         return;
      rc_work = (uint32 *) (tables + bytes * 4);
      rc_stack = rc_work + RC_WORK;
      rc_fix = (rc_fix_t *) (rc_stack + RC_WORK);
      rc.at = (uint8 *) (rc_fix + RC_WORK);
      rc_lead = rc.at + bytes;
      rc_later = (rc_later_t *) (rc_lead + bytes);
      rc_later_room = RC_WORK;
      rc.table = (uint32 *) tables;
   }
   rc.prg = prg;
   rc.size = bytes;
   rc.z = 0xFFFFFFFF;
   if (fixed)
   {
      rc.direct = mmap(NULL, 0x10004 * sizeof(uint32), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (MAP_FAILED == (void *) rc.direct)
         rc.direct = NULL;
   }
   rc_fixed = fixed && rc.direct;
   rc.pages = cpu.mem_page;
   /* a cartridge's own memory at 0x6000, when no handler of anybody's is there: the pages' */
   {
      nes6502_memread *mr;
      nes6502_memwrite *mw;

      rc.paged_read = rc.paged_write = 0x6000;
      for (mr = cpu.read_handler; mr->min_range != 0xFFFFFFFF; mr++)
         if (mr->max_range >= 0x6000 && mr->min_range < 0x8000)
            rc.paged_read = 0x8000;
      for (mw = cpu.write_handler; mw->min_range != 0xFFFFFFFF; mw++)
         if (mw->max_range >= 0x6000 && mw->min_range < 0x8000)
            rc.paged_write = 0x8000;
   }
}

void nes6502_recompiled(unsigned *instructions, unsigned *bytes, unsigned *runs, unsigned *entries)
{
   *instructions = rc_count;
   *bytes = (unsigned) ((char *) rc_p - (char *) rc_code);
   *runs = rc_runs;
   *entries = rc_entries;
}

/* The translator's memory a worker that runs the 6502 will write, for the first core to touch
** first: its tables, and `code` bytes of its code from where it has got to. */
void nes6502_recompile_touch(void (*touch)(void *at, unsigned bytes), unsigned code)
{
   uint32 left = (uint32) ((char *) rc_end - (char *) rc_p);

   if (!rc.table)
      return;
   touch(rc.table, (uint32) ((uint8 *) (rc_later + RC_WORK) - (uint8 *) rc.table));
   if (rc.direct)
      touch(rc.direct, 0x10004 * sizeof(uint32));
   touch(rc_p, code < left ? code : left);
}
