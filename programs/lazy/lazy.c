// The machine's parallel fill and copy (CSRs 0x0b0-0x0b3) against plain loops, as a program
// that runs in place of the kernel: supervisor mode at 0x82000000, console through the SBI
// call, as rvc_cpu and the harness start our Linux image (tools\make_lazy_image.py makes the
// image's folder). The shader machine may leave a fill or a copy to its commit pass and run on
// (LAZY_MEMOP, docs/machine/boot.md); every case here is one where that could show. It prints a line a
// case and a sum, which must be the same on rvc_cpu and on the shader machine.
#include <stdint.h>

#define PAGE 4096
#define WORDS (2 * PAGE / 4)

static uint32_t table[1024] __attribute__((aligned(PAGE)));          // the page table: megapages, as they lie
static uint32_t table2[1024] __attribute__((aligned(PAGE)));         // four small pages (case 10)
static uint32_t table3[1024] __attribute__((aligned(PAGE)));         // a table that is filled (case 11)
static uint32_t a[WORDS] __attribute__((aligned(PAGE)));
static uint32_t b[WORDS] __attribute__((aligned(PAGE)));
static uint32_t c[WORDS] __attribute__((aligned(PAGE)));
static uint32_t want[WORDS];
static uint32_t run_here[PAGE / 4] __attribute__((aligned(PAGE)));   // a page of code is copied here and run

static void putc(char ch) {
    register uint32_t a0 __asm__("a0") = (uint8_t)ch, a7 __asm__("a7") = 1;
    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7) : "a1", "memory");
}
static void puts(const char* s) { while (*s) putc(*s++); }
static void put_hex(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) putc("0123456789abcdef"[(v >> i) & 15]);
}

static void fill(void* to, uint32_t word, uint32_t bytes) {
    __asm__ volatile("csrw 0x0b1, %0\n csrw 0x0b2, %1\n csrw 0x0b3, %2\n li t0, 2\n csrw 0x0b0, t0" : : "r"(word), "r"(to), "r"(bytes) : "t0", "memory");
}
static void copy(void* to, const void* from, uint32_t bytes) {
    __asm__ volatile("csrw 0x0b1, %0\n csrw 0x0b2, %1\n csrw 0x0b3, %2\n li t0, 1\n csrw 0x0b0, t0" : : "r"(from), "r"(to), "r"(bytes) : "t0", "memory");
}
static void next_pass(void) { __asm__ volatile(".word 0x0100000f"); }   // pause: what was stored is committed

static uint32_t seed = 2463534242u, sum;
static uint32_t random(void) {
    seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
    return seed;
}
static void pattern(uint32_t* p) {
    for (int i = 0; i < WORDS; i++) p[i] = random();
}
// `got` against `want`, folded into the sum; the case's line
static void check(const char* name, const volatile uint32_t* got) {
    int wrong = 0;
    for (int i = 0; i < WORDS; i++) {
        uint32_t v = got[i];
        wrong += v != want[i];
        sum = (sum << 5 | sum >> 27) ^ v;
    }
    puts("lazy: ");
    puts(name);
    puts(wrong ? " WRONG " : " ok ");
    put_hex((uint32_t)wrong);
    putc('\n');
}

// A page of code that depends on nothing but where it is called.
__attribute__((naked, aligned(PAGE), section(".text.probe"))) static uint32_t probe(uint32_t x) {
    __asm__("slli a1, a0, 3\n add a0, a0, a1\n addi a0, a0, 42\n ret\n .balign 4096");
}

int main(void) {
    for (int i = 0; i < 1024; i++) table[i] = 0;
    for (uint32_t m = 0x200; m < 0x220; m++) table[m] = m << 20 | 0xcf;   // 0x80000000-0x87ffffff as it lies: V R W X A D
    // and b and c once more at 0x40000000, as four pages of 4 KiB (case 10)
    table[0x100] = ((uint32_t)table2 >> 12) << 10 | 1;
    table[0x102] = 0x40000000u >> 2 | 0xcf;   // the ROM's first 4 MiB at 0x40800000 (case 12)
    for (int i = 0; i < 1024; i++) table2[i] = 0;
    for (uint32_t i = 0; i < 4; i++) table2[i] = ((((uint32_t)(i < 2 ? b : c)) >> 12) + (i & 1)) << 10 | 0xcf;
    __asm__ volatile("csrw satp, %0\n sfence.vma" : : "r"(0x80000000u | (uint32_t)table >> 12));

    // 1: a fill, read at once
    pattern(b);
    next_pass();
    fill(b, 0x5a5a5a5au, sizeof b);
    for (int i = 0; i < WORDS; i++) want[i] = 0x5a5a5a5au;
    check("fill then read", b);

    // 2: a fill, then stores into it, then read
    next_pass();
    fill(c, 0, sizeof c);
    for (int i = 0; i < WORDS; i++) want[i] = 0;
    for (int i = 0; i < 300; i++) {
        uint32_t at = random() % WORDS, v = random();
        ((volatile uint32_t*)c)[at] = v;
        want[at] = v;
    }
    ((volatile uint8_t*)c)[5] = 0x77;
    want[1] = (want[1] & ~0xff00u) | 0x7700;
    check("fill, store, read", c);

    // 3: a copy, then stores to its source, then the destination and the source read
    pattern(a);
    next_pass();
    copy(b, a, sizeof a);
    for (int i = 0; i < WORDS; i++) want[i] = a[i];
    for (int i = 0; i < 200; i++) ((volatile uint32_t*)a)[random() % WORDS] = random();
    check("copy, store to the source, read the copy", b);
    for (int i = 0; i < WORDS; i++) want[i] = a[i];
    next_pass();
    check("the source after its stores", a);

    // 4: stores to the destination and to the source in the same pass, before the copy
    next_pass();
    for (int i = 0; i < 100; i++) ((volatile uint32_t*)c)[random() % WORDS] = random();
    for (int i = 0; i < 100; i++) ((volatile uint32_t*)a)[random() % WORDS] = random();
    copy(c, a, sizeof a);
    for (int i = 0; i < WORDS; i++) want[i] = a[i];
    check("store to both, then copy", c);

    // 5: stores into a copy's destination after it, a few bytes among them
    next_pass();
    copy(b, a, sizeof a);
    for (int i = 0; i < WORDS; i++) want[i] = a[i];
    for (int i = 0; i < 150; i++) {
        uint32_t at = random() % (WORDS * 4), v = random() & 0xff;
        ((volatile uint8_t*)b)[at] = (uint8_t)v;
        want[at / 4] = (want[at / 4] & ~(0xffu << (at % 4 * 8))) | v << (at % 4 * 8);
    }
    check("copy, then bytes stored into it", b);

    // 6: two in a row, then a third from the first one's destination
    next_pass();
    fill(b, 0x11111111u, sizeof b);
    fill(c, 0x22222222u, sizeof c);
    for (int i = 0; i < WORDS; i++) want[i] = 0x11111111u;
    check("two fills: the first", b);
    for (int i = 0; i < WORDS; i++) want[i] = 0x22222222u;
    check("two fills: the second", c);
    next_pass();
    fill(b, 0x33333333u, sizeof b);
    ((volatile uint32_t*)b)[7] = 0x12345678;
    copy(c, b, sizeof b);
    for (int i = 0; i < WORDS; i++) want[i] = 0x33333333u;
    want[7] = 0x12345678;
    check("a fill, a store, a copy of it", c);

    // 7: sizes and places that are not whole texels
    next_pass();
    pattern(a);
    fill(c, 0, sizeof c);
    next_pass();
    copy((uint8_t*)c + 4, (uint8_t*)a + 4, 1000);
    for (int i = 0; i < WORDS; i++) want[i] = i >= 1 && i < 251 ? a[i] : 0;
    check("a copy of 1000 bytes from the second word", c);

    // the same two read only after the pass has ended: what the commit made of them
    next_pass();
    pattern(a);
    next_pass();
    copy(b, a, sizeof a);
    next_pass();
    for (int i = 0; i < WORDS; i++) want[i] = a[i];
    check("copy, read a pass later", b);
    next_pass();
    fill(c, 0x44444444u, sizeof c);
    ((volatile uint32_t*)c)[3] = 9;
    next_pass();
    for (int i = 0; i < WORDS; i++) want[i] = 0x44444444u;
    want[3] = 9;
    check("fill and a store, read a pass later", c);

    // 8: a page of code copied, then run where it was copied to
    next_pass();
    copy(run_here, (void*)probe, PAGE);
    uint32_t ran = ((uint32_t (*)(uint32_t))run_here)(7);
    sum = (sum << 5 | sum >> 27) ^ ran;
    puts("lazy: code copied and run ");
    puts(ran == 7 * 9 + 42 ? "ok " : "WRONG ");
    put_hex(ran);
    putc('\n');
    // and a fill over the page being run from would be the end of this program: not tried

    // 9: many small ones with work between, as a program's start has them
    next_pass();
    for (int k = 0; k < 64; k++) {
        uint32_t* to = k & 1 ? b : c;
        if (k % 3 == 0) fill(to, k * 0x01010101u, sizeof b);
        else copy(to, a, sizeof a);
        for (int i = 0; i < 40; i++) ((volatile uint32_t*)to)[random() % WORDS] ^= random();
        for (int i = 0; i < WORDS; i += 64) sum = (sum << 5 | sum >> 27) ^ to[i];
        for (int i = 0; i < 20; i++) ((volatile uint32_t*)a)[random() % WORDS] = random();
    }
    for (int i = 0; i < WORDS; i++) want[i] = b[i];
    check("sixty-four with work between", b);

    // 10: a destination reached through pages of 4 KiB that were mapped before it was asked
    // for, in this pass and in the one before (a machine that leaves the fill to its commit
    // has translations of them to forget)
    volatile uint32_t* b_small = (volatile uint32_t*)0x40000000;
    volatile uint32_t* c_small = (volatile uint32_t*)0x40002000;
    pattern(b);
    pattern(c);
    next_pass();
    sum ^= c_small[11];
    c_small[12] = c_small[12];
    next_pass();
    sum ^= b_small[5];
    b_small[6] = b_small[6];
    fill(b, 0x66666666u, sizeof b);
    copy(c, a, sizeof a);
    b_small[9] = 0x99;
    c_small[1030] = 0x77;
    for (int i = 0; i < WORDS; i++) want[i] = 0x66666666u;
    want[9] = 0x99;
    check("a fill through small pages mapped before it", b_small);
    for (int i = 0; i < WORDS; i++) want[i] = a[i];
    want[1030] = 0x77;
    check("a copy through small pages mapped a pass before", c_small);
    next_pass();
    check("the same a pass later", c_small);

    // 11: a page table that is a fill's destination, written and walked in the same pass
    for (int i = 0; i < 1024; i++) table3[i] = random() & ~1u;
    next_pass();
    fill(table3, 0, PAGE);
    table3[1] = ((uint32_t)a >> 12) << 10 | 0xcf;
    table3[2] = (((uint32_t)a >> 12) + 1) << 10 | 0xcf;
    table[0x101] = ((uint32_t)table3 >> 12) << 10 | 1;
    __asm__ volatile("sfence.vma");
    for (int i = 0; i < WORDS; i++) want[i] = a[i];
    check("a page table that was just filled", (volatile uint32_t*)0x40401000);
    sum ^= table3[0] | table3[3] | table3[1023];

    // 12: a copy from the ROM (mapped at 0x40800000), stored into and read at once, and a pass later
    const volatile uint32_t* rom = (const volatile uint32_t*)(0x40800000 + PAGE);
    next_pass();
    copy(b, (const void*)rom, sizeof b);
    ((volatile uint32_t*)b)[5] ^= 0xff;
    for (int i = 0; i < WORDS; i++) want[i] = rom[i];
    want[5] ^= 0xff;
    check("a copy from the ROM, a store into it", b);
    next_pass();
    ((volatile uint32_t*)b)[9] ^= 0x100;   // (and so that the two do not cancel in the sum)
    want[9] ^= 0x100;
    check("the same a pass later, and one more store", b);

    puts("lazy: sum ");
    put_hex(sum);
    puts(" done\n");
    for (;;) next_pass();
}
