// The machine as a bare-metal program sees it: RAM from 0x80000000, a 16550 UART, and a display
// that is nothing but RAM at fixed addresses (docs/display.md).
#pragma once

#include <stdint.h>

#define UART_THR (*(volatile uint8_t*)0x10000000)
#define UART_LSR (*(volatile uint8_t*)0x10000005)
#define UART_LSR_THRE 0x20

#define UART_RBR UART_THR                 // same register: write to send, read to receive
#define UART_LSR_DATA 0x01

// Waits on the status register only: reading UART_THR would take a pending input character.
static inline void uart_putc(char c) {
    while (!(UART_LSR & UART_LSR_THRE)) {}
    UART_THR = (uint8_t)c;
}

// Blocks until a character arrives.
static inline char uart_getc(void) {
    while (!(UART_LSR & UART_LSR_DATA)) {}
    return (char)UART_RBR;
}

static inline void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}

// Gives up the rest of the current emulator frame. Use it when waiting for something that can
// only change between frames (the GPU, the timer), instead of spinning.
static inline void cpu_wait(void) {
    __asm__ volatile("wfi");
}

// Free-running timer (low word of the CLINT's mtime).
#define CLINT_MTIME (*(volatile uint32_t*)0x0200bff8)
#define CLINT_HZ 5000

// Display control words. Write width and height first, the mode last.
#define DISP_MODE   (*(volatile uint32_t*)0x87000000)
#define DISP_WIDTH  (*(volatile uint32_t*)0x87000004)
#define DISP_HEIGHT (*(volatile uint32_t*)0x87000008)
#define DISP_MODE_OFF     0
#define DISP_MODE_RGB32   1   // one word per pixel, 0x00RRGGBB
#define DISP_MODE_INDEXED 2   // one byte per pixel, an index into DISP_PALETTE
#define DISP_MODE_GPU     3   // show what the GPU device drew (gpu.h) instead of DISP_PIXELS

#define DISP_PALETTE ((volatile uint32_t*)0x87000400)   // 256 words, 0x00RRGGBB
#define DISP_PIXELS  0x87001000u                        // rows top to bottom, no padding
