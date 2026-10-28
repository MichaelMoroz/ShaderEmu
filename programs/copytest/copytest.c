// The machine's parallel copy (CSRs 0x0b0-0x0b3) from RAM to RAM, from the ROM, and its fill,
// at lengths over the shader machine's chunk of 256 KB (docs/machine/gpu.md, "RAM in one
// texture"), as a program in the kernel's place (supervisor mode at 0x80400000, no paging,
// the console as words in memory; tools\make_lazy_image.py --program copytest makes its folder).
// Every case is checked against plain loops; the sum must be the same on rvc_cpu and the shader.
#include <stdint.h>

// the console (docs/machine/console.md): bytes written so far, and the ring of 256; a line a pass
static void putc(char ch) {
    volatile uint32_t* written = (volatile uint32_t*)0x876b8294u;
    uint32_t n = *written;
    *(volatile uint8_t*)(0x876b82a0u + (n & 255)) = (uint8_t)ch;
    *written = n + 1;
}
static void puts(const char* s) { while (*s) putc(*s++); }
static void put_hex(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) putc("0123456789abcdef"[(v >> i) & 15]);
}
static void put_dec(uint32_t v) {
    char b[12];
    int n = 0;
    do b[n++] = (char)('0' + v % 10); while (v /= 10);
    while (n) putc(b[--n]);
}

static void fill(uint32_t to, uint32_t word, uint32_t bytes) {
    __asm__ volatile("csrw 0x0b1, %0\n csrw 0x0b2, %1\n csrw 0x0b3, %2\n li t0, 2\n csrw 0x0b0, t0" : : "r"(word), "r"(to), "r"(bytes) : "t0", "memory");
}
static void copy(uint32_t to, uint32_t from, uint32_t bytes) {
    __asm__ volatile("csrw 0x0b1, %0\n csrw 0x0b2, %1\n csrw 0x0b3, %2\n li t0, 1\n csrw 0x0b0, t0" : : "r"(from), "r"(to), "r"(bytes) : "t0", "memory");
}
static void next_pass(void) { __asm__ volatile(".word 0x0100000f"); }   // pause: what was stored is committed
static uint32_t passes(void) { return *(volatile uint32_t*)0x8700032cu; }   // the control words' count of passes

#define W(a) (*(volatile uint32_t*)(a))
#define SCRATCH 0x8a000000u   // what the destination should be, made by plain loops
#define GUARD 64u             // bytes checked on each side of a destination

static uint32_t seed = 2463534242u, sum;
static uint32_t random(void) {
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return seed;
}
static void pattern(uint32_t at, uint32_t bytes) {
    for (uint32_t k = 0; k < bytes; k += 4) W(at + k) = random();
}
// `bytes` at `at` (with GUARD on each side) against SCRATCH, folded into the sum; the case's line
static void check(const char* name, uint32_t at, uint32_t bytes, uint32_t took) {
    uint32_t wrong = 0;
    for (uint32_t k = 0; k < bytes + 2 * GUARD; k += 4) {
        uint32_t v = W(at - GUARD + k);
        wrong += v != W(SCRATCH + k);
        sum = (sum << 5 | sum >> 27) ^ v;
    }
    puts("copytest: ");
    puts(name);
    puts(wrong ? " WRONG " : " ok ");
    put_hex(wrong);
    puts(" (");
    put_dec(took);
    puts(" passes)\n");
}
// The case's want: the destination's surroundings as they are, then the copy made by a loop
// (through a buffer, so that an overlap is a memmove's).
static void want_copy(uint32_t to, uint32_t from, uint32_t bytes) {
    for (uint32_t k = 0; k < bytes + 2 * GUARD; k += 4) W(SCRATCH + k) = W(to - GUARD + k);
    uint32_t buffer = SCRATCH + 0x01000000u;
    for (uint32_t k = 0; k < bytes; k += 4) W(buffer + k) = W(from + k);
    for (uint32_t k = 0; k < bytes; k += 4) W(SCRATCH + GUARD + k) = W(buffer + k);
}
static void case_copy(const char* name, uint32_t to, uint32_t from, uint32_t bytes) {
    pattern(from, bytes);
    pattern(to - GUARD, bytes + 2 * GUARD);
    if (from < to + bytes + GUARD && to - GUARD < from + bytes) pattern(from, bytes);   // (an overlap: the source last)
    want_copy(to, from, bytes);
    next_pass();
    uint32_t before = passes();
    copy(to, from, bytes);
    check(name, to, bytes, passes() - before);
}

int main(void) {
#ifndef PROGRAM_COPY_ONLY   // (-DPROGRAM_COPY_ONLY: case 10 alone, a quick look on WARP)
    // 1-2: one chunk, and four
    case_copy("4 KB", 0x83000000u, 0x83100000u, 4096);
    case_copy("1 MB forward", 0x83000000u, 0x83400000u, 0x100000);
    // 3-4: a destination inside the source ahead of it (made from its end), and behind it
    case_copy("overlap ahead", 0x83010000u, 0x83000000u, 0x60000);
    case_copy("overlap behind", 0x83000000u, 0x83010000u, 0x60000);
    // 5: the source off a texel and the length off one too, over three chunks
    case_copy("odd source", 0x83800000u, 0x83a00004u, 0x80008);
    // 6: from below 0x88000000 to above it, and across it
    case_copy("across 0x88", 0x8c000000u, 0x87f00000u, 0x200000);
    case_copy("onto 0x88", 0x87f80000u, 0x84000000u, 0x100000);
    // 7: from the ROM, 1 MB at once
    {
        uint32_t to = 0x84800000u, bytes = 0x100000;
        pattern(to - GUARD, bytes + 2 * GUARD);
        want_copy(to, 0x40000000u, bytes);
        next_pass();
        uint32_t before = passes();
        copy(to, 0x40000000u, bytes);
        check("ROM 1 MB", to, bytes, passes() - before);
    }
    // 8: a fill of 2 MB
    {
        uint32_t to = 0x85000000u, bytes = 0x200000;
        pattern(to - GUARD, bytes + 2 * GUARD);
        for (uint32_t k = 0; k < bytes + 2 * GUARD; k += 4) W(SCRATCH + k) = k < GUARD || k >= GUARD + bytes ? W(to - GUARD + k) : 0x5a5aa5a5u;
        next_pass();
        uint32_t before = passes();
        fill(to, 0x5a5aa5a5u, bytes);
        check("fill 2 MB", to, bytes, passes() - before);
    }
    // 9: a store to the destination's texel just before it, in the copy's own pass
    {
        uint32_t to = 0x83000004u, from = 0x83200000u, bytes = 1000;
        pattern(from, bytes);
        pattern(to - GUARD, bytes + 2 * GUARD);
        want_copy(to, from, bytes);
        W(SCRATCH + GUARD - 4) = 0x12345678u;
        next_pass();
        W(to - 4) = 0x12345678u;
        uint32_t before = passes();
        copy(to, from, bytes);
        check("store beside", to, bytes, passes() - before);
    }
    puts("copytest: sum ");
    put_hex(sum);
    puts(" done\n");
#endif
    // 10 (after the sum, which stays as it was): the same copy by a program's CSRs (0x8b0-0x8b3, docs/machine/gpu.md,
    // "A program's copy"), with bytes stored before it in its pass as memcpy's head does; 0x8b0 reads 1 on the first core
    {
        uint32_t to = 0x83000000u, from = 0x83100000u, bytes = 8192, worth;
        pattern(from, bytes);
        pattern(to - GUARD, bytes + 2 * GUARD);
        want_copy(to, from, bytes);
        W(SCRATCH + GUARD - 4) = 0x9abcdef0u;
        next_pass();
        __asm__ volatile("csrr %0, 0x8b0" : "=r"(worth));
        W(to - 4) = 0x9abcdef0u;
        uint32_t before = passes();
        __asm__ volatile("csrw 0x8b1, %0\n csrw 0x8b2, %1\n csrw 0x8b3, %2\n li t0, 1\n csrw 0x8b0, t0" : : "r"(from), "r"(to), "r"(bytes) : "t0", "memory");
        check(worth == 1 ? "program's copy" : "program's copy, 0x8b0 not 1:", to, bytes, passes() - before);
    }
    for (;;) next_pass();
}
