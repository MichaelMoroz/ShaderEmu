// Float instructions on the fast path, as a bare-metal program: loads and stores of floats,
// arithmetic, comparisons, conversions and moves on a fixed pseudo-random sequence, folded into
// one sum that is printed. The sum has no meaning of its own: it must be the same before and
// after a change to how the tick shader runs the F extension (run it with --machine full, the
// machine that has it).
//
//   clang --target=riscv32-unknown-elf -march=rv32imaf -mabi=ilp32 ... (programs\build.bat)
#include "../common/platform.h"

static volatile float table[64];
static volatile uint32_t words[64];

static void put_hex(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) uart_putc("0123456789abcdef"[(v >> i) & 15]);
}

static inline uint32_t bits_of(float f) {
    union { float f; uint32_t u; } x;
    x.f = f;
    return x.u;
}

int main(void) {
    __asm__ volatile("li t0, 0x2000\n csrs mstatus, t0");   // FS = initial: float instructions allowed
    uint32_t seed = 12345, sum = 0;
    for (int i = 0; i < 64; i++) {
        seed = seed * 1664525u + 1013904223u;
        table[i] = (float)(int32_t)(seed >> 8) * (1.0f / 4096.0f) - 1000.0f;
        words[i] = seed;
    }
    for (int round = 0; round < 20000; round++) {
        seed = seed * 1664525u + 1013904223u;
        int a = (seed >> 3) & 63, b = (seed >> 11) & 63, c = (seed >> 19) & 63;
        float x = table[a], y = table[b];
        float r;
        switch ((seed >> 26) & 7) {
            case 0: r = x + y; break;
            case 1: r = x - y; break;
            case 2: r = x * y * (1.0f / 1024.0f); break;
            case 3: r = y != 0.0f ? x / y : x; break;
            case 4: r = x < y ? x : y; break;
            case 5: r = (float)(int32_t)words[c] * (1.0f / 65536.0f); break;
            case 6: r = (float)(uint32_t)(words[c] >> 4); break;
            default: r = -x + (x <= y ? 1.5f : -2.25f); break;
        }
        if (r > 1.0e9f || r < -1.0e9f) r = r * (1.0f / 1048576.0f);
        table[c] = r;
        int32_t back = (int32_t)(r * 16.0f);
        words[a] = (uint32_t)back ^ bits_of(r);
        sum = (sum << 5 | sum >> 27) ^ bits_of(r) ^ (uint32_t)back ^ (uint32_t)(x == y) ^ ((uint32_t)(x >= y) << 1);
    }
    uart_puts("fpcheck: ");
    put_hex(sum);
    uart_puts(" done\n");
    for (;;) cpu_wait();
}
