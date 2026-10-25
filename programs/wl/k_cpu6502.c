/*
 * cpu6502: an emulator's interpreter loop, as the NES's runs on gpu_vm (nofrendo's nes6502.c: on the RISC-V
 * machine its code is translated instead, linux/nes/rc6502.h). Re-implemented in nofrendo's manner: the
 * opcode fetched through a table of 4 KB pages, a switch over the opcodes, the flags kept apart (N and Z as
 * the last result, C, V), memory read and written through the pages, cycles counted down. The program is
 * made here: a frame's game logic stand-in (sprite tables walked with indexed and indirect loads, adds and
 * shifts, compares, branches, a subroutine a sprite, stores to a zero page and a sprite page). A unit: the
 * program from its start until it has run 120,000 cycles (four NES frames).
 */
#include <string.h>
#include "wl.h"

#define CYCLES 120000

struct cpu {
    uint8_t ram[0x800], sprites[0x100], work[0x2000];
    uint8_t *pages[16];
    uint32_t pc;
    uint8_t a, x, y, s, n_z, c, v, i, d;
    int32_t remaining;
};

static uint8_t rom[0x1000];
static const uint8_t cycles_of[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7};

static inline uint8_t mem_read(struct cpu *c, uint32_t address) { return c->pages[address >> 12][address & 0xfff]; }
static inline void mem_write(struct cpu *c, uint32_t address, uint8_t value)
{
    if (address < 0x2000) c->ram[address & 0x7ff] = value;
    else if (address >= 0x6000 && address < 0x8000) c->work[address & 0x1fff] = value;
    else if (address == 0x4014) memcpy(c->sprites, c->ram + ((uint32_t)value << 8 & 0x700), 0x100);   /* a sprite DMA */
}
static inline uint32_t fetch16(struct cpu *c, uint32_t address) { return mem_read(c, address) | (uint32_t)mem_read(c, address + 1) << 8; }
static inline uint32_t zp16(struct cpu *c, uint8_t at) { return c->ram[at] | (uint32_t)c->ram[(uint8_t)(at + 1)] << 8; }

#define SET_NZ(value) (c->n_z = (value))
#define PUSH(value) (c->ram[0x100 + c->s--] = (value))
#define PULL() (c->ram[0x100 + ++c->s])

static void run_cpu(struct cpu *c)
{
    while (c->remaining > 0) {
        uint8_t op = mem_read(c, c->pc), m;
        uint32_t address, t;
        c->pc++;
        c->remaining -= cycles_of[op];
        switch (op) {
        case 0xa9: c->a = mem_read(c, c->pc++); SET_NZ(c->a); break;                                   /* lda # */
        case 0xa5: c->a = c->ram[mem_read(c, c->pc++)]; SET_NZ(c->a); break;                           /* lda zp */
        case 0xb5: c->a = c->ram[(uint8_t)(mem_read(c, c->pc++) + c->x)]; SET_NZ(c->a); break;         /* lda zp,x */
        case 0xbd: address = fetch16(c, c->pc) + c->x; c->pc += 2; c->a = mem_read(c, address); SET_NZ(c->a); break;   /* lda abs,x */
        case 0xb9: address = fetch16(c, c->pc) + c->y; c->pc += 2; c->a = mem_read(c, address); SET_NZ(c->a); break;   /* lda abs,y */
        case 0xb1: address = zp16(c, mem_read(c, c->pc++)) + c->y; c->a = mem_read(c, address); SET_NZ(c->a); break;   /* lda (zp),y */
        case 0xa2: c->x = mem_read(c, c->pc++); SET_NZ(c->x); break;                                   /* ldx # */
        case 0xa0: c->y = mem_read(c, c->pc++); SET_NZ(c->y); break;                                   /* ldy # */
        case 0x85: c->ram[mem_read(c, c->pc++)] = c->a; break;                                         /* sta zp */
        case 0x95: c->ram[(uint8_t)(mem_read(c, c->pc++) + c->x)] = c->a; break;                       /* sta zp,x */
        case 0x9d: address = fetch16(c, c->pc) + c->x; c->pc += 2; mem_write(c, address, c->a); break; /* sta abs,x */
        case 0x91: address = zp16(c, mem_read(c, c->pc++)) + c->y; mem_write(c, address, c->a); break; /* sta (zp),y */
        case 0x8d: address = fetch16(c, c->pc); c->pc += 2; mem_write(c, address, c->a); break;        /* sta abs */
        case 0x69:                                                                                       /* adc # */
        case 0x75:                                                                                       /* adc zp,x */
        case 0x79:                                                                                       /* adc abs,y */
            m = op == 0x69 ? mem_read(c, c->pc++) : op == 0x75 ? c->ram[(uint8_t)(mem_read(c, c->pc++) + c->x)]
                                                               : mem_read(c, (fetch16(c, c->pc) + c->y) & 0xffff);
            if (op == 0x79) c->pc += 2;
            t = c->a + m + c->c;
            c->v = (~(c->a ^ m) & (c->a ^ t) & 0x80) != 0;
            c->c = t > 0xff;
            c->a = (uint8_t)t;
            SET_NZ(c->a);
            break;
        case 0xe9: m = mem_read(c, c->pc++); t = c->a - m - (1 - c->c);                                /* sbc # */
            c->v = ((c->a ^ m) & (c->a ^ t) & 0x80) != 0; c->c = t < 0x100; c->a = (uint8_t)t; SET_NZ(c->a); break;
        case 0x29: c->a &= mem_read(c, c->pc++); SET_NZ(c->a); break;                                  /* and # */
        case 0x09: c->a |= mem_read(c, c->pc++); SET_NZ(c->a); break;                                  /* ora # */
        case 0x49: c->a ^= mem_read(c, c->pc++); SET_NZ(c->a); break;                                  /* eor # */
        case 0x0a: c->c = c->a >> 7; c->a <<= 1; SET_NZ(c->a); break;                                  /* asl a */
        case 0x4a: c->c = c->a & 1; c->a >>= 1; SET_NZ(c->a); break;                                   /* lsr a */
        case 0x2a: t = (uint32_t)(c->a << 1 | c->c); c->c = c->a >> 7; c->a = (uint8_t)t; SET_NZ(c->a); break;   /* rol a */
        case 0xc9: t = c->a - mem_read(c, c->pc++); c->c = t < 0x100; SET_NZ((uint8_t)t); break;       /* cmp # */
        case 0xe0: t = c->x - mem_read(c, c->pc++); c->c = t < 0x100; SET_NZ((uint8_t)t); break;       /* cpx # */
        case 0xc0: t = c->y - mem_read(c, c->pc++); c->c = t < 0x100; SET_NZ((uint8_t)t); break;       /* cpy # */
        case 0xe8: c->x++; SET_NZ(c->x); break;                                                         /* inx */
        case 0xc8: c->y++; SET_NZ(c->y); break;                                                         /* iny */
        case 0xca: c->x--; SET_NZ(c->x); break;                                                         /* dex */
        case 0x88: c->y--; SET_NZ(c->y); break;                                                         /* dey */
        case 0xe6: m = mem_read(c, c->pc++); c->ram[m]++; SET_NZ(c->ram[m]); break;                    /* inc zp */
        case 0xaa: c->x = c->a; SET_NZ(c->x); break;                                                    /* tax */
        case 0x8a: c->a = c->x; SET_NZ(c->a); break;                                                    /* txa */
        case 0xa8: c->y = c->a; SET_NZ(c->y); break;                                                    /* tay */
        case 0x18: c->c = 0; break;                                                                     /* clc */
        case 0x38: c->c = 1; break;                                                                     /* sec */
        case 0xd0: case 0xf0: case 0x10: case 0x30: case 0x90: case 0xb0: {                             /* branches */
            int8_t off = (int8_t)mem_read(c, c->pc++);
            int take = op == 0xd0 ? c->n_z != 0 : op == 0xf0 ? c->n_z == 0 : op == 0x10 ? !(c->n_z & 0x80) : op == 0x30 ? (c->n_z & 0x80) != 0
                     : op == 0x90 ? !c->c : c->c;
            if (take) {
                c->remaining -= 1 + (((c->pc + off) ^ c->pc) & 0x100 ? 1 : 0);
                c->pc = (c->pc + (uint32_t)off) & 0xffff;
            }
            break;
        }
        case 0x4c: c->pc = fetch16(c, c->pc); break;                                                    /* jmp */
        case 0x20: address = fetch16(c, c->pc); t = c->pc + 1; PUSH((uint8_t)(t >> 8)); PUSH((uint8_t)t); c->pc = address; break;   /* jsr */
        case 0x60: t = PULL(); t |= (uint32_t)PULL() << 8; c->pc = t + 1; break;                        /* rts */
        case 0x48: PUSH(c->a); break;                                                                   /* pha */
        case 0x68: c->a = PULL(); SET_NZ(c->a); break;                                                  /* pla */
        default: break;                                                                                 /* nop */
        }
    }
}

/* the program at 0xf000: for each of 64 sprites, a subroutine moves it by its speed, bounces it, and copies it */
static void setup(void)
{
    static const uint8_t program[] = {
        0xa2, 0x00,             /* f000 ldx #0 */
        0x8a,                   /* f002 txa */
        0x0a, 0x0a,             /* f003 asl a; asl a */
        0xa8,                   /* f005 tay */
        0x20, 0x40, 0xf0,       /* f006 jsr f040 */
        0xe8,                   /* f009 inx */
        0xe0, 0x40,             /* f00a cpx #64 */
        0xd0, 0xf4,             /* f00c bne f002 */
        0xa9, 0x02,             /* f00e lda #2 */
        0x8d, 0x14, 0x40,       /* f010 sta 4014 (sprite DMA from 0x200) */
        0xe6, 0x10,             /* f013 inc 10 */
        0x4c, 0x00, 0xf0,       /* f015 jmp f000 */
    };
    static const uint8_t sprite[] = {
        0xb9, 0x00, 0x03,       /* f040 lda 300,y   (x position) */
        0x18,                   /* clc */
        0x79, 0x01, 0x03,       /* adc 301,y        (speed) */
        0xc9, 0xf0,             /* cmp #f0 */
        0x90, 0x08,             /* bcc +8 */
        0xa9, 0x00,             /* lda #0 */
        0x38,                   /* sec */
        0xe9, 0x01,             /* sbc #1 */
        0x29, 0x7f,             /* and #7f */
        0x49, 0x05,             /* eor #5 */
        0x91, 0x20,             /* sta (20),y */
        0xb1, 0x22,             /* lda (22),y       (a tile from a table) */
        0x4a,                   /* lsr a */
        0x2a,                   /* rol a */
        0x75, 0x30,             /* adc 30,x */
        0x95, 0x30,             /* sta 30,x */
        0x9d, 0x00, 0x02,       /* sta 200,x */
        0x48, 0x68,             /* pha; pla */
        0xc8, 0xc8,             /* iny; iny */
        0x60,                   /* rts */
    };
    memset(rom, 0xea, sizeof rom);
    memcpy(rom, program, sizeof program);
    memcpy(rom + 0x40, sprite, sizeof sprite);
}

static void *inst(void) { return wl_alloc(sizeof(struct cpu)); }

static uint32_t run(void *ctx)
{
    struct cpu *c = ctx;
    uint32_t seed = 0x6502, sum = 0;
    memset(c, 0, sizeof *c);
    for (int p = 0; p < 2; p++) c->pages[p] = c->ram;   /* (mirrors: each page reads the 2 KB of RAM; enough here) */
    for (int p = 2; p < 6; p++) c->pages[p] = c->ram;
    c->pages[6] = c->work, c->pages[7] = c->work + 0x1000;
    for (int p = 8; p < 16; p++) c->pages[p] = rom;
    for (int i = 0; i < 0x800; i++) c->ram[i] = (uint8_t)wl_rand(&seed);
    c->ram[0x20] = 0x00, c->ram[0x21] = 0x04, c->ram[0x22] = 0x00, c->ram[0x23] = 0x05;
    c->pc = 0xf000, c->s = 0xff, c->remaining = CYCLES;
    run_cpu(c);
    for (int i = 0; i < 0x800; i += 7) sum = sum * 31 + c->ram[i];
    return sum + c->pc + c->a;
}

const wl_kernel wl_cpu6502 = {"cpu6502", "NES: a 6502 interpreter's loop in nofrendo's manner, 120,000 cycles", setup, inst, run};
