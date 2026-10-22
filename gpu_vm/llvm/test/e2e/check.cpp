// Compares the code LLVM made for hello.s or pack.s (linked with ld.lld) with what gpu_vm/ref/gvm_asm.h makes for
// the same program, bundle for bundle: check hello|pack PROGRAM.elf
#include "../../../ref/gvm_asm.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::vector<unsigned char> img;
static uint32_t rd32(size_t o) { uint32_t v; memcpy(&v, &img[o], 4); return v; }
static uint16_t rd16(size_t o) { uint16_t v; memcpy(&v, &img[o], 2); return v; }

struct Elf {
    uint32_t text_addr = 0, text_off = 0, text_size = 0;
    uint32_t sym(const char *name) const {
        uint32_t shoff = rd32(0x20), shentsize = rd16(0x2e), shnum = rd16(0x30);
        for (uint32_t i = 0; i < shnum; i++) {
            size_t sh = shoff + i * shentsize;
            if (rd32(sh + 4) != 2) continue;   // SHT_SYMTAB
            size_t str = shoff + rd32(sh + 24) * shentsize, stroff = rd32(str + 16);
            for (uint32_t s = 0; s < rd32(sh + 20) / 16; s++) {
                size_t e = rd32(sh + 16) + s * 16;
                if (!strcmp((const char *)&img[stroff + rd32(e)], name)) return rd32(e + 4);
            }
        }
        fprintf(stderr, "check: no symbol %s\n", name);
        exit(1);
    }
    void load() {
        uint32_t shoff = rd32(0x20), shentsize = rd16(0x2e), shnum = rd16(0x30), shstrndx = rd16(0x32);
        size_t names = rd32(shoff + shstrndx * shentsize + 16);
        for (uint32_t i = 0; i < shnum; i++) {
            size_t sh = shoff + i * shentsize;
            if (!strcmp((const char *)&img[names + rd32(sh)], ".text")) {
                text_addr = rd32(sh + 12); text_off = rd32(sh + 16); text_size = rd32(sh + 20);
            }
        }
    }
};

enum { ZERO = 0, RA = 1, A0 = 5, A1 = 6, A2 = 7, A7 = 12, T0 = 13 };

struct Patch { size_t word; uint32_t value; };
static std::vector<Patch> patches;

static void hello(GvmAsm &a, const Elf &e) {
    a.li(A7, 64);
    a.li(A0, 1);
    a.I(GVM_OP_II(GVM_I_ADD), A1, ZERO, (int32_t)e.sym("msg"));
    a.li(A2, 14);
    a.x(GVM_X_SYSCALL);
    a.li(A7, 93);
    a.li(A0, 0);
    a.x(GVM_X_SYSCALL);
}

static void pack(GvmAsm &a, const Elf &e) {
    const uint32_t t0 = T0, t1 = T0 + 1, t2 = T0 + 2, t3 = T0 + 3, t4 = T0 + 4, t5 = T0 + 5, t6 = T0 + 6, t7 = T0 + 7,
                   t8 = T0 + 8, t9 = T0 + 9, t10 = T0 + 10, t11 = T0 + 11;
    int loop = a.label(), func = a.label(), far = a.label();
    a.I(GVM_OP_II(GVM_I_ADD), t0, ZERO, (int32_t)e.sym("arr"));
    a.li(t1, 10);
    a.li(A0, 0);
    a.bind(loop);
    a.lw(t2, t0, 0);
    a.add(A0, A0, t2);
    a.bump(t0, 4);
    a.addbnz(t1, -1, loop);
    a.I(GVM_OP_II(GVM_I_ADD), t3, ZERO, (int32_t)e.sym("buf"));
    a.sw(A0, t3, 0);
    a.lw(t4, t3, 4);
    a.lw(t5, t3, 8);
    a.lw(t6, t5, 0);
    a.sw(t6, t3, 12);
    a.li(t7, 0x3fc00000);
    a.fopi(GVM_F_ADD, t8, t7, 2.0f);
    a.fop(GVM_F_MUL, t9, t8, t8);
    a.fop(GVM_F_FTOI, t10, t9, 0);
    a.add(A0, A0, t10);
    a.call(func);
    a.put2(GVM_WORD_I(GVM_X_LPC, t11, 0u, 0u), 0);
    size_t lpc_bundle = a.w.size() / 4;
    a.j(far);
    for (int i = 0; i < 600; i++) a.add(ZERO, ZERO, ZERO);
    a.bind(far);
    a.li(A7, 93);
    a.x(GVM_X_SYSCALL);
    a.bind(func);
    a.addi(A0, A0, 1);
    a.ret();
    // lpc of a label of the data: its long immediate, from the bundle (gvm_asm's lpc takes its own labels only)
    patches.push_back({lpc_bundle * 4 + 2, e.sym("arr") - (a.base + (uint32_t)lpc_bundle * 16)});
}

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: check hello|pack PROGRAM.elf\n"); return 1; }
    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    int c;
    while ((c = fgetc(f)) != EOF) img.push_back((unsigned char)c);
    fclose(f);
    Elf e;
    e.load();
    GvmAsm a(e.text_addr);
    std::string what = argv[1];
    if (what == "hello") hello(a, e);
    else if (what == "pack") pack(a, e);
    else { fprintf(stderr, "check: no case %s\n", argv[1]); return 1; }
    std::vector<uint32_t> want = a.finish();
    for (const Patch &p : patches) want[p.word] = p.value;
    uint32_t words = e.text_size / 4;
    int bad = 0;
    if (words != want.size()) { printf("%s: %u words of .text, gvm_asm made %zu\n", argv[1], words, want.size()); bad++; }
    for (uint32_t i = 0; i < words && i < want.size(); i++) {
        uint32_t got = rd32(e.text_off + 4 * i);
        if (got != want[i] && bad++ < 20)
            printf("%s: bundle %u word %u: %08x, gvm_asm %08x\n", argv[1], i / 4, i % 4, got, want[i]);
    }
    printf("%s: %zu bundles, %s\n", argv[1], want.size() / 4, bad ? "DIFFERENT" : "the same as gvm_asm.h's");
    return bad != 0;
}
