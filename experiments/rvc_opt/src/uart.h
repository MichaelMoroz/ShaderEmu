#ifndef UART_H
#define UART_H


// RAM_ADDR / RAM_LIN: see types.h
#define RAM_MAX (2048 * (4096 - 64) * 4 * 4)


/* shift by two to ignore byte offset */
// The write cache holds RAM texels (four words), not words: stores come in runs (a register
// save, a structure, a copy), and a run then takes one entry. A bucket is the tags of its
// L1_WAYS entries in one state texel (texel number + 1; 0 is free) and then their texels.
// There are two tables of 2^L1_TABLE_BITS buckets with different hashes; a texel goes to the
// second only when its bucket in the first is full, so most lookups read one bucket. Entries
// are filled in order and never freed within a pass.
//   L1_TABLE_BITS 6, L1_WAYS 3 (the default): 384 texels, an array of 512 in the tick
//   L1_TABLE_BITS 6, L1_WAYS 4:               512 texels, arrays of 512 and 128
//   L1_TABLE_BITS 7, L1_WAYS 3:               768 texels, an array of 1,024
// The first is 5% more instructions a second than the second in spite of its stalls: four
// entries a bucket cost every load and store, and more than about 1,000 state texels (each is
// a pixel that runs the whole tick) cost every instruction.
#ifndef L1_BUCKETS
#ifndef L1_TABLE_BITS
#define L1_TABLE_BITS 6
#endif
#ifndef L1_WAYS
#define L1_WAYS 3
#endif
#define L1_TABLE (1 << L1_TABLE_BITS)
#define L1_BUCKETS (2 * L1_TABLE)
#define L1_STRIDE (L1_WAYS + 1)
#define L1_ENTRIES (L1_BUCKETS * L1_STRIDE)
#if defined(CORES) && CORES > 1
// A worker core's write cache is as large as the machine's geometry says (docs/multicore.md,
// "The geometry"): tables of 2^mc_bits_now buckets, in the first buckets of the same arrays.
// mc_bits_now is the size of the core whose cache is being looked at, in the tick and in
// the commit.
#define L1_NOW_BITS mc_bits_now
#define L1_NOW_TABLE (1u << L1_NOW_BITS)
#define L1_B0(t) ((t) & (L1_NOW_TABLE - 1))
#define L1_B1(t) (L1_NOW_TABLE + ((((t) >> 3) ^ ((t) << (L1_NOW_BITS - 3)) ^ ((t) >> L1_NOW_BITS)) & (L1_NOW_TABLE - 1)))
#else
#define L1_B0(t) ((t) & (L1_TABLE - 1))
#define L1_B1(t) (L1_TABLE + ((((t) >> 3) ^ ((t) << (L1_TABLE_BITS - 3)) ^ ((t) >> L1_TABLE_BITS)) & (L1_TABLE - 1)))
#endif
#endif


#define SHIFT_RBR 0
#define SHIFT_THR 8
#define SHIFT_IER 16
#define SHIFT_IIR 24
#define SHIFT_LCR 0
#define SHIFT_MCR 8
#define SHIFT_LSR 16
#define SHIFT_SCR 24

#define UART_GET1(x) ((cpu.uart.rbr_thr_ier_iir >> SHIFT_##x) & 0xff)
#define UART_GET2(x) ((cpu.uart.lcr_mcr_lsr_scr >> SHIFT_##x) & 0xff)

#define UART_SET1(x, val) cpu.uart.rbr_thr_ier_iir = (cpu.uart.rbr_thr_ier_iir & (~(0xff << SHIFT_##x))) | (val << SHIFT_##x)
#define UART_SET2(x, val) cpu.uart.lcr_mcr_lsr_scr = (cpu.uart.lcr_mcr_lsr_scr & (~(0xff << SHIFT_##x))) | (val << SHIFT_##x)

#define IER_RXINT_BIT 0x1
#define IER_THREINT_BIT 0x2

#define IIR_THR_EMPTY 0x2
#define IIR_RD_AVAILABLE 0x4
#define IIR_NO_INTERRUPT 0x7

#define LSR_DATA_AVAILABLE 0x1
#define LSR_THR_EMPTY 0x20

// The host has sent a character this machine has not put in the receive register yet.
#if CORES > 1
#define UART_INPUT_WAITING (hart == 0 && cpu.uart.input_tag != _UdonUARTInTag)   // input is core 0's
#else
#define UART_INPUT_WAITING (cpu.uart.input_tag != _UdonUARTInTag)
#endif

void uart_update_iir() {
    bool rx_ip = (UART_GET1(IER) & IER_RXINT_BIT) != 0 && UART_GET1(RBR) != 0;
    bool thre_ip = (UART_GET1(IER) & IER_THREINT_BIT) != 0 && UART_GET1(THR) == 0;
    UART_SET1(IIR, (rx_ip ? IIR_RD_AVAILABLE : (thre_ip ? IIR_THR_EMPTY : IIR_NO_INTERRUPT)));
}

void put_byte_to_fb(uint c) {
    cpu.uart_buffer.ptr++;
    if (cpu.uart_buffer.ptr == 63) {
        cpu.stall = STALL_UART;
    }
    [forcecase]
    switch (cpu.uart_buffer.ptr) {
case 0: cpu.uart_buffer.buf0 = c; break;
case 1: cpu.uart_buffer.buf1 = c; break;
case 2: cpu.uart_buffer.buf2 = c; break;
case 3: cpu.uart_buffer.buf3 = c; break;
case 4: cpu.uart_buffer.buf4 = c; break;
case 5: cpu.uart_buffer.buf5 = c; break;
case 6: cpu.uart_buffer.buf6 = c; break;
case 7: cpu.uart_buffer.buf7 = c; break;
case 8: cpu.uart_buffer.buf8 = c; break;
case 9: cpu.uart_buffer.buf9 = c; break;
case 10: cpu.uart_buffer.buf10 = c; break;
case 11: cpu.uart_buffer.buf11 = c; break;
case 12: cpu.uart_buffer.buf12 = c; break;
case 13: cpu.uart_buffer.buf13 = c; break;
case 14: cpu.uart_buffer.buf14 = c; break;
case 15: cpu.uart_buffer.buf15 = c; break;
case 16: cpu.uart_buffer.buf16 = c; break;
case 17: cpu.uart_buffer.buf17 = c; break;
case 18: cpu.uart_buffer.buf18 = c; break;
case 19: cpu.uart_buffer.buf19 = c; break;
case 20: cpu.uart_buffer.buf20 = c; break;
case 21: cpu.uart_buffer.buf21 = c; break;
case 22: cpu.uart_buffer.buf22 = c; break;
case 23: cpu.uart_buffer.buf23 = c; break;
case 24: cpu.uart_buffer.buf24 = c; break;
case 25: cpu.uart_buffer.buf25 = c; break;
case 26: cpu.uart_buffer.buf26 = c; break;
case 27: cpu.uart_buffer.buf27 = c; break;
case 28: cpu.uart_buffer.buf28 = c; break;
case 29: cpu.uart_buffer.buf29 = c; break;
case 30: cpu.uart_buffer.buf30 = c; break;
case 31: cpu.uart_buffer.buf31 = c; break;
case 32: cpu.uart_buffer.buf32 = c; break;
case 33: cpu.uart_buffer.buf33 = c; break;
case 34: cpu.uart_buffer.buf34 = c; break;
case 35: cpu.uart_buffer.buf35 = c; break;
case 36: cpu.uart_buffer.buf36 = c; break;
case 37: cpu.uart_buffer.buf37 = c; break;
case 38: cpu.uart_buffer.buf38 = c; break;
case 39: cpu.uart_buffer.buf39 = c; break;
case 40: cpu.uart_buffer.buf40 = c; break;
case 41: cpu.uart_buffer.buf41 = c; break;
case 42: cpu.uart_buffer.buf42 = c; break;
case 43: cpu.uart_buffer.buf43 = c; break;
case 44: cpu.uart_buffer.buf44 = c; break;
case 45: cpu.uart_buffer.buf45 = c; break;
case 46: cpu.uart_buffer.buf46 = c; break;
case 47: cpu.uart_buffer.buf47 = c; break;
case 48: cpu.uart_buffer.buf48 = c; break;
case 49: cpu.uart_buffer.buf49 = c; break;
case 50: cpu.uart_buffer.buf50 = c; break;
case 51: cpu.uart_buffer.buf51 = c; break;
case 52: cpu.uart_buffer.buf52 = c; break;
case 53: cpu.uart_buffer.buf53 = c; break;
case 54: cpu.uart_buffer.buf54 = c; break;
case 55: cpu.uart_buffer.buf55 = c; break;
case 56: cpu.uart_buffer.buf56 = c; break;
case 57: cpu.uart_buffer.buf57 = c; break;
case 58: cpu.uart_buffer.buf58 = c; break;
case 59: cpu.uart_buffer.buf59 = c; break;
case 60: cpu.uart_buffer.buf60 = c; break;
case 61: cpu.uart_buffer.buf61 = c; break;
case 62: cpu.uart_buffer.buf62 = c; break;
case 63: cpu.uart_buffer.buf63 = c; break;

    }
}

void uart_tick() {
    //bool rx_ip = false;

    if ((cpu.clock & 0xff) == 0xff) {
        if (cpu.uart.input_tag != _UdonUARTInTag && UART_GET1(RBR) == 0) {
            // Burst input: _UdonUARTInChar carries up to four characters, first in the low byte,
            // and the host advances the tag by their count. One is handed over each time the
            // receive register is free, so the guest's console poll reads a whole escape
            // sequence at once instead of one byte per poll. A single character with the tag
            // advanced by one behaves exactly as upstream.
            uint in_count = (_UdonUARTInChar >> 24) ? 4 : ((_UdonUARTInChar >> 16) ? 3 : ((_UdonUARTInChar >> 8) ? 2 : 1));
            uint in_left = _UdonUARTInTag - cpu.uart.input_tag;
            uint in_char;
            if (in_left > in_count) {
                in_char = _UdonUARTInChar & 0xff;
                cpu.uart.input_tag = _UdonUARTInTag;
            } else {
                in_char = (_UdonUARTInChar >> (8 * (in_count - in_left))) & 0xff;
                cpu.uart.input_tag++;
            }
            if (in_char != 0) {
                UART_SET1(RBR, in_char);
                UART_SET2(LSR, (UART_GET2(LSR) | LSR_DATA_AVAILABLE));
                uart_update_iir();
                //if ((UART_GET1(IER) & IER_RXINT_BIT) != 0) {
                //    rx_ip = true;
                //}
            }
        }
    }

    uint thr = UART_GET1(THR);
    if (thr != 0) {
        put_byte_to_fb(thr);
        UART_SET1(THR, 0);
        UART_SET2(LSR, (UART_GET2(LSR) | LSR_THR_EMPTY));
        uart_update_iir();
        if ((UART_GET1(IER) & IER_THREINT_BIT) != 0) {
            cpu.uart.thre_ip = true;
        }
    }

    // if (!cpu.uart.interrupting && (cpu.uart.thre_ip || rx_ip)) {
    //     cpu.uart.interrupting = true;
    //     cpu.uart.thre_ip = false;
    // } else {
    //     cpu.uart.interrupting = false;
    // }
}

#endif
