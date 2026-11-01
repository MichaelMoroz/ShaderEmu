// Every operation rv2udon.py translates, over edge values, into RAM and a printed sum: the
// translation must give the reference's console and RAM to the word (README.md).
#include "../../programs/common/platform.h"

static volatile int32_t vals[] = {0, 1, -1, 2, -2, 7, -7, 31, 32, 33, 0x7fffffff, (int32_t)0x80000000,
                                  0x12345678, (int32_t)0xdeadbeef, 0xffff, 0x8000, 255, 128};
#define NV (int)(sizeof(vals) / sizeof(vals[0]))
static int32_t out[NV * NV * 24];
static volatile uint8_t bytes[64];
static volatile int32_t atom;

static void put_hex(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) uart_putc("0123456789abcdef"[(v >> i) & 15]);
}

int main(void) {
    int n = 0;
    uint32_t sum = 0;
    for (int i = 0; i < NV; i++)
        for (int j = 0; j < NV; j++) {
            int32_t a = vals[i], b = vals[j];
            uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
            int32_t r[24] = {
                a + b, a - b, a ^ b, a | b, a & b, a << (b & 31), a >> (b & 31), (int32_t)(ua >> (ub & 31)),
                a < b, ua < ub, a * b,
                (int32_t)(((int64_t)a * b) >> 32), (int32_t)(((uint64_t)ua * ub) >> 32),
                (int32_t)(((int64_t)a * (int64_t)ub) >> 32),
                b == 0 ? -1 : (a == (int32_t)0x80000000 && b == -1) ? a : a / b,
                b == 0 ? -1 : (int32_t)(ua / ub),
                b == 0 ? a : (a == (int32_t)0x80000000 && b == -1) ? 0 : a % b,
                b == 0 ? a : (int32_t)(ua % ub),
                a < 5, ua < 5u, a >> 3, (int32_t)(ua >> 3), a ^ 0x5a5, a == 0,
            };
            // the C above keeps division by zero and MIN / -1 apart; these run the instructions
            int32_t q, rem;
            __asm__ volatile("div %0, %2, %3\n rem %1, %2, %3" : "=&r"(q), "=&r"(rem) : "r"(a), "r"(b));
            uint32_t uq, ur;
            __asm__ volatile("divu %0, %2, %3\n remu %1, %2, %3" : "=&r"(uq), "=&r"(ur) : "r"(ua), "r"(ub));
            r[14] ^= q;      // 0 when the instruction agrees with the C
            r[15] ^= (int32_t)uq;
            r[16] ^= rem;
            r[17] ^= (int32_t)ur;
            for (int k = 0; k < 24; k++) { out[n++] = r[k]; sum = sum * 31 + (uint32_t)r[k]; }
        }
    // bytes and halves, signed and not, at every offset
    for (int i = 0; i < 64; i++) bytes[i] = (uint8_t)(i * 37 + 11);
    for (int i = 0; i < 60; i++) {
        volatile int8_t* sb = (volatile int8_t*)&bytes[i];
        volatile int16_t* sh = (volatile int16_t*)&bytes[i & ~1];
        volatile uint16_t* uh = (volatile uint16_t*)&bytes[i & ~1];
        sum = sum * 31 + (uint32_t)*sb;
        sum = sum * 31 + bytes[i];
        sum = sum * 31 + (uint32_t)*sh;
        sum = sum * 31 + *uh;
        *uh = (uint16_t)(*uh + 0x1234);
        bytes[i + 1] ^= (uint8_t)sum;
    }
    for (int i = 0; i < 64; i++) sum = sum * 31 + bytes[i];
    // atomics, one hart
    atom = 5;
    sum = sum * 31 + (uint32_t)__atomic_fetch_add(&atom, 7, __ATOMIC_SEQ_CST);
    sum = sum * 31 + (uint32_t)__atomic_exchange_n(&atom, 100, __ATOMIC_SEQ_CST);
    sum = sum * 31 + (uint32_t)__atomic_fetch_or(&atom, 3, __ATOMIC_SEQ_CST);
    int32_t expect = 103;
    sum = sum * 31 + (uint32_t)__atomic_compare_exchange_n(&atom, &expect, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    sum = sum * 31 + (uint32_t)atom;
    uart_puts("optest ");
    put_hex(sum);
    uart_puts(" done\r\n");
    for (;;) cpu_wait();
}
