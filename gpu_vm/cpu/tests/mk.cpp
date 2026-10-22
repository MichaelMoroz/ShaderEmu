// gvm_cpu's tests before the compiler: small programs assembled with gpu_vm/ref/gvm_asm.h into ELF executables (and one
// raw image), for tests/run.sh.   mk OUTDIR
#define _CRT_SECURE_NO_WARNINGS
#include "../../ref/gvm_asm.h"
#include <string>

static const uint32_t CODE = GVM_RAM, DATA = GVM_RAM + 0x10000, BSS = GVM_RAM + 0x20000;
enum { SYS_READ = 63, SYS_WRITE = 64, SYS_EXIT = 93 };

struct Seg { uint32_t at; std::vector<uint8_t> bytes; uint32_t memsz; };

static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i))); }
static void put16(std::vector<uint8_t>& v, uint32_t x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }

static void write_file(const std::string& path, const std::vector<uint8_t>& b) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "mk: cannot write %s\n", path.c_str()); exit(1); }
    fwrite(b.data(), 1, b.size(), f);
    fclose(f);
}
static std::vector<uint8_t> words(const std::vector<uint32_t>& w) { std::vector<uint8_t> b; for (uint32_t x : w) put32(b, x); return b; }

// ELF32, little-endian, ET_EXEC, EM_GVM: a PT_LOAD for each segment
static void write_elf(const std::string& path, uint32_t entry, const std::vector<Seg>& segs) {
    std::vector<uint8_t> e = {0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint32_t off = 52 + 32 * (uint32_t)segs.size();
    put16(e, 2); put16(e, EM_GVM); put32(e, 1); put32(e, entry); put32(e, 52); put32(e, 0); put32(e, 0);
    put16(e, 52); put16(e, 32); put16(e, (uint32_t)segs.size()); put16(e, 40); put16(e, 0); put16(e, 0);
    for (const Seg& s : segs) {
        put32(e, 1); put32(e, off); put32(e, s.at); put32(e, s.at); put32(e, (uint32_t)s.bytes.size());
        put32(e, s.memsz > s.bytes.size() ? s.memsz : (uint32_t)s.bytes.size());   // (0: the bytes' size)
        put32(e, 7); put32(e, 16);
        off += (uint32_t)s.bytes.size();
    }
    for (const Seg& s : segs) e.insert(e.end(), s.bytes.begin(), s.bytes.end());
    write_file(path, e);
}

static void sys(GvmAsm& a, uint32_t n) { a.li(12, (int32_t)n); a.x(GVM_X_SYSCALL); }
static void exit_with(GvmAsm& a, uint32_t r) { a.mv(5, r); sys(a, SYS_EXIT); a.halt(); }

int main(int argc, char** argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    auto path = [&](const char* n) { return out + "/" + n; };

    {   // hello: write(1, msg, len), then exit(0); the op after each syscall shares its bundle (the run resumes there)
        const char msg[] = "Hello from gpu_vm!\n";
        GvmAsm a(CODE);
        a.li(5, 1); a.li(6, (int32_t)DATA); a.li(7, (int32_t)sizeof msg - 1);
        a.li(12, SYS_WRITE); a.close();
        a.x(GVM_X_SYSCALL); a.li(5, 0);
        a.li(12, SYS_EXIT); a.close();
        a.x(GVM_X_SYSCALL); a.halt();
        std::vector<uint8_t> d(msg, msg + sizeof msg - 1);
        write_elf(path("hello.elf"), CODE, {{CODE, words(a.finish()), 0}, {DATA, d, (uint32_t)d.size()}});
    }
    for (int raw = 0; raw < 2; raw++) {   // sum: 100 + 99 + ... + 1 by addbnz, a call, a word through the bss; exit(5050 & 255 = 186)
        GvmAsm a(CODE);
        int top = a.label(), fn = a.label();
        a.li(20, 0); a.li(21, 100);
        a.bind(top);
        a.add(20, 20, 21);
        a.addbnz(21, -1, top);
        a.call(fn);
        a.li(8, (int32_t)BSS); a.close();
        a.lw(9, 8, 64);                      // the bss's: 0
        a.add(20, 20, 9);
        a.sw(20, 8, 0); a.close();
        a.lw(10, 8, 0);                      // read back (the core's own store, before the commit)
        exit_with(a, 10);
        a.bind(fn);                          // fn: r20 += 0 through a sub and an add
        a.addi(20, 20, 7); a.addi(20, 20, -7);
        a.ret();
        std::vector<uint32_t> w = a.finish();
        if (raw) write_file(path("sum.bin"), words(w));
        else write_elf(path("sum.elf"), CODE, {{CODE, words(w), 0}, {BSS, {}, 4096}});
    }
    {   // opasync: a fill and a copy of what it fills asked for back to back, with no sync: neither waits for the other
        // (made in the order asked for), and the core stops only where they are at work: a load of the copy's
        // destination, a store to its source. Exit 7 when all agree.
        GvmAsm a(CODE);
        a.li(29, (int32_t)GVM_OPS); a.li(28, (int32_t)BSS); a.li(27, (int32_t)DATA);
        a.li(20, GVM_OP_FILL); a.li(21, (int32_t)BSS); a.li(22, 0x01010101); a.li(23, 64);
        a.st(GVM_W_Q, 20, 29, 0);            // the fill of BSS, no sync
        a.li(20, GVM_OP_COPY); a.li(21, (int32_t)(BSS + 0x100)); a.li(22, (int32_t)BSS); a.li(23, 64);
        a.st(GVM_W_Q, 20, 29, 0);            // a copy of what the fill makes, at once
        a.li(10, 0); a.li(11, 50);
        int spin = a.here();
        a.add(10, 10, 11); a.addbnz(11, -1, spin);   // other work meanwhile
        a.li(8, (int32_t)0xdeadbeef); a.close();
        a.sw(8, 28, 0);                      // to the copy's source: waits until both are made
        a.lw(9, 28, 0x13c);                  // the copied word: the fill's
        a.lw(7, 28, 0x100);                  // and the first: the fill's, not the store's
        a.lw(6, 29, 0);                      // in flight: 0
        a.li(30, 0);
        a.li(12, 0x01010101); a.op(GVM_I_SEQ, 13, 9, 12); a.add(30, 30, 13);
        a.op(GVM_I_SEQ, 13, 7, 12); a.opi(GVM_I_SLL, 13, 13, 1); a.add(30, 30, 13);
        a.op(GVM_I_SEQ, 13, 6, 0); a.opi(GVM_I_SLL, 13, 13, 2); a.add(30, 30, 13);
        exit_with(a, 30);
        write_elf(path("opasync.elf"), CODE, {{CODE, words(a.finish()), 0}, {BSS, {}, 4096}});
    }
    {   // echo: read(0, buf, 64), write(1, buf, n), exit(n)
        GvmAsm a(CODE);
        a.li(5, 0); a.li(6, (int32_t)BSS); a.li(7, 64); sys(a, SYS_READ);
        a.mv(20, 5);
        a.li(5, 1); a.li(6, (int32_t)BSS); a.mv(7, 20); sys(a, SYS_WRITE);
        exit_with(a, 20);
        write_elf(path("echo.elf"), CODE, {{CODE, words(a.finish()), 0}, {BSS, {}, 4096}});
    }
    {   // args: write(1, argv[1], strlen), a newline, exit(argc)
        GvmAsm a(CODE);
        int top = a.label(), done = a.label();
        a.mv(30, 5);
        a.lw(21, 6, 4); a.close();
        a.mv(20, 21); a.li(23, 0);
        a.bind(top);
        a.ld(GVM_W_BU, 22, 21, 0); a.close();   // (a branch reads the registers as its bundle found them)
        a.br(GVM_CC_BEQI, 22, 0, done);
        a.addi(21, 21, 1); a.addi(23, 23, 1);
        a.j(top);
        a.bind(done);
        a.li(5, 1); a.mv(6, 20); a.mv(7, 23); sys(a, SYS_WRITE);
        a.li(5, 1); a.li(6, (int32_t)DATA); a.li(7, 1); sys(a, SYS_WRITE);
        exit_with(a, 30);
        write_elf(path("args.elf"), CODE, {{CODE, words(a.finish()), 0}, {DATA, {'\n'}, 1}});
    }
    {   // fault: a load past RAM's end, with no handler
        GvmAsm a(CODE);
        a.li(8, 0x70000000); a.lw(9, 8, 0);
        exit_with(a, 0);
        write_elf(path("fault.elf"), CODE, {{CODE, words(a.finish()), 0}});
    }
    {   // break, with no handler
        GvmAsm a(CODE);
        a.li(5, 7); a.halt();
        write_elf(path("break.elf"), CODE, {{CODE, words(a.finish()), 0}});
    }
    {   // handler: with TVEC set a syscall is the program's (twice: r40 counts), then TVEC 0 and exit(r40 = 2)
        GvmAsm a(CODE);
        int handler = a.label(), go = a.label();
        a.lpc(5, handler); a.mtsr(GVM_SR_TVEC, 5);
        a.j(go);
        a.bind(handler);
        a.addi(40, 40, 1); a.mfsr(51, GVM_SR_EPC); a.close();
        a.addi(51, 51, 4); a.mtsr(GVM_SR_EPC, 51);
        a.eret();
        a.bind(go);
        a.li(12, 999); a.x(GVM_X_SYSCALL); a.x(GVM_X_SYSCALL);
        a.mtsr(GVM_SR_TVEC, 0);
        exit_with(a, 40);
        write_elf(path("handler.elf"), CODE, {{CODE, words(a.finish()), 0}});
    }
    {   // timer: TCMP at 0.5 s, wait in a loop; the interrupt's handler exits with 42 + (TIME >= 50000)
        GvmAsm a(CODE);
        int handler = a.label(), spin = a.label();
        a.lpc(5, handler); a.mtsr(GVM_SR_TVEC, 5);
        a.li(6, 50000); a.mtsr(GVM_SR_TCMP_LO, 6);
        a.li(6, 1 << GVM_IRQ_TIMER); a.mtsr(GVM_SR_IEN, 6);
        a.li(6, GVM_ST_IE); a.mtsr(GVM_SR_STATUS, 6);
        a.bind(spin);
        a.x(GVM_X_WAIT);
        a.j(spin);
        a.bind(handler);
        a.mtsr(GVM_SR_TVEC, 0); a.mfsr(7, GVM_SR_TIME_LO); a.close();
        a.li(8, 50000); a.close();
        a.op(GVM_I_SGEU, 7, 7, 8);
        a.addi(7, 7, 42);
        exit_with(a, 7);
        write_elf(path("timer.elf"), CODE, {{CODE, words(a.finish()), 0}});
    }
    {   // spin: a loop forever (for --max-bundles)
        GvmAsm a(CODE);
        int top = a.here();
        a.addi(5, 5, 1);
        a.j(top);
        write_elf(path("spin.elf"), CODE, {{CODE, words(a.finish()), 0}});
    }
    return 0;
}
