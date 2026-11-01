// ---------------------------------------------------------------- library routines run natively
// musl's and the kernel's memcpy, memmove, memset and libgcc's 64-bit division, found by their words, become
// Buffer.BlockCopy, Array.Clear and Int64 division; a case they do not cover falls into the translated body.
enum { N_MEMCPY, N_MEMSET, N_UDIV, N_UMOD, N_SDIV, N_SMOD, N_STRCMP, N_MEMCMP };
struct NSig { const char* name; int kind; bool user; int n; u32 hash, first; };
static const NSig NSIGS[] = {
    {"memcpy", N_MEMCPY, true, 105, 0x46a2a7cau, 0x00b562b3u},
    {"memcpy_any", N_MEMCPY, true, 312, 0xe60dfd39u, 0x0035f793u},
    {"memset", N_MEMSET, true, 66, 0x3ce134ffu, 0x10060263u},
    {"memmove", N_MEMCPY, true, 99, 0x9d9bb898u, 0x00050693u},
    {"__udivdi3", N_UDIV, true, 131, 0xadca4f2cu, 0xfe010113u},
    {"__umoddi3", N_UMOD, true, 114, 0xfd4e5877u, 0xfe010113u},
    {"__divdi3", N_SDIV, true, 30, 0x9427f59fu, 0x41f5d793u},
    {"__moddi3", N_SMOD, true, 31, 0xd07d6b24u, 0xfe010113u},
    {"strcmp", N_STRCMP, true, 8, 0x0e8c8dd1u, 0x00054783u},
    {"memcmp", N_MEMCMP, true, 12, 0xe0de6e31u, 0x02060063u},
    {"k_memcpy", N_MEMCPY, false, 101, 0xe83fef72u, 0x40000293u},
    {"k_memset", N_MEMSET, false, 87, 0xe7b8bef4u, 0x000012b7u},
    {"k_memmove", N_MEMCPY, false, 14, 0x9e2639efu, 0x1ab50c63u},
};
static const string MAX = "SystemMath.__Max__SystemInt32_SystemInt32__SystemInt32";
static bool NATIVE = true;                 // L2U_NATIVE=0: translate them as they are
static i64 NATMIN = 16;                    // L2U_NATMIN=N: lengths under N run the translated body
static map<string, int> nat_found;

static void native_scan(const vector<u32>& w) {
    NAT.clear();
    if (!NATIVE) return;
    for (size_t i = 0; i < w.size(); i++)
        for (const NSig& s : NSIGS) {
            if (w[i] != s.first || s.user != S->user || i + s.n > w.size()) continue;
            u32 h = 2166136261u;
            for (int k = 0; k < s.n; k++) { u32 v = w[i + k]; if ((v & 0x7F) == 0x6F) v &= 0xFFF; h = (h ^ v) * 16777619u; }
            if (h == s.hash) { NAT[S->lo + 4 * (i64)i] = s.kind; nat_found[s.name]++; }
        }
}

static void nat_ret(Asm& o, i64 pc) {          // return to ra as the translated ret does
    Item it; it.d = decode(0x00008067); it.pc = pc; it.form = K_JALR; it.A = x(1); it.B = x(0);
    t_jalr(o, it);
}

static void emit_native(Asm& o, int kind, i64 pc) {
    auto c = [&](i64 v) { return o.c(v); };
    string orig = o.fresh(), ret = o.fresh();
    auto when = [&](const string& cmp, const string& a, const string& b, const string& to) {   // jump to `to` if a cmp b
        string no = o.fresh(); o.ex(cmp, {a, b, "cnd"}); o.jif("cnd", no); o.jump(to); o.L(no);
    };
    if (kind == N_MEMCPY || kind == N_MEMSET) {
        when(LT, "x12", c(NATMIN), orig);                     // small or "negative" lengths: the body
        o.ex(ADD, {"x10", c(MIN), "t"}); when(LT, "t", "x0", orig);
        o.ex(SUB, {c(RAMSIZE), "t", "t3"}); when(GT, "x12", "t3", orig);
        if (DIRTY) { o.ex(ADD, {"t", "x12", "mhi"}); o.ex(SUB, {"mhi", c(1), "mhi"}); mark_range(o, "t", "mhi"); }
        if (kind == N_MEMCPY) {
            string rom = o.fresh(), go = o.fresh();
            o.ex(ADD, {"x11", c(MIN), "t2"}); when(LT, "t2", "x0", rom);
            o.ex(SUB, {c(RAMSIZE), "t2", "t3"}); when(GT, "x12", "t3", orig);
            o.ex(BLOCKCOPY, {"mem", "t2", "mem", "t", "x12"}); o.jump(go);
            o.L(rom);
            o.ex(SUB, {"x11", c(ROM), "t2"}); when(LT, "t2", "x0", orig);
            o.ex(SHL, {"romlen", c(2), "t3"}); o.ex(SUB, {"t3", "t2", "t3"}); when(GT, "x12", "t3", orig);
            o.ex(BLOCKCOPY, {"rom", "t2", "mem", "t", "x12"});
            o.L(go);
            o.ex(SHR, {"x12", c(1), "t"});
        } else {
            string slow = o.fresh(), loop = o.fresh(), done = o.fresh(), fit = o.fresh();
            o.ex(AND, {"x11", c(255), "t2"});
            o.ex(OR, {"t", "x12", "t3"}); o.ex(AND, {"t3", c(3), "t3"}); o.ex(OR, {"t3", "t2", "t3"}); when(NE, "t3", "x0", slow);
            o.ex(SHR, {"t", c(2), "t"}); o.ex(SHR, {"x12", c(2), "t2"}); o.ex(ACLEAR, {"mem", "t", "t2"}); o.jump(done);
            o.L(slow);                                         // one byte, then doubling copies of what is set
            o.ex(TOB, {"t2", "bt"}); o.ex(SETB, {"mem", "t", "bt"}); o.copy(c(1), "ii");
            o.L(loop);
            when(GE, "ii", "x12", done);
            o.ex(SUB, {"x12", "ii", "t3"}); when(LT, "t3", "ii", fit); o.copy("ii", "t3"); o.L(fit);
            o.ex(ADD, {"t", "ii", "t2"}); o.ex(BLOCKCOPY, {"mem", "t", "mem", "t2", "t3"}); o.ex(ADD, {"ii", "t3", "ii"}); o.jump(loop);
            o.L(done);
            o.ex(SHR, {"x12", c(2), "t"});
        }
        o.ex(ADD, {"t", c(16), "t"}); o.ex(SUB, {"gleft", "t", "gleft"});
        nat_ret(o, pc);
        o.L(orig);
        return;
    }
    if (kind == N_STRCMP || kind == N_MEMCMP) {          // words while both are aligned and equal, then bytes
        bool mc = kind == N_MEMCMP;
        string wl = o.fresh(), bl = o.fresh(), bytes = o.fresh(), diff = o.fresh(), same = o.fresh(), fin = o.fresh();
        o.ex(ADD, {"x10", c(MIN), "t"}); when(LT, "t", "x0", orig);
        o.ex(ADD, {"x11", c(MIN), "t2"}); when(LT, "t2", "x0", orig);
        if (mc) { when(LT, "x12", "x0", orig); o.copy("x12", "ii"); }
        else { o.ex(MAX, {"t", "t2", "ii"}); o.ex(SUB, {c(RAMSIZE), "ii", "ii"}); }   // strcmp: bytes before the end
        if (mc) { o.ex(MAX, {"t", "t2", "t3"}); o.ex(ADD, {"t3", "ii", "t3"}); when(GT, "t3", c(RAMSIZE), orig); }
        o.copy("ii", "sh");                                  // sh: what is left, for the charge
        o.ex(OR, {"t", "t2", "t3"}); o.ex(AND, {"t3", c(3), "t3"}); when(NE, "t3", "x0", bytes);
        o.ex(SHR, {"t", c(2), "idx"}); o.ex(SHR, {"t2", c(2), "u"});
        o.L(wl);
        when(LT, "ii", c(4), bytes);
        o.ex(AGET, {"mem", "idx", "w"}); o.ex(AGET, {"mem", "u", "t3"}); when(NE, "w", "t3", bytes);
        if (!mc) {
            o.ex(SUB, {"w", c(0x01010101), "t3"}); o.ex(XOR, {"w", c(MASK), "w"}); o.ex(AND, {"t3", "w", "t3"});
            o.ex(AND, {"t3", c((i64)0x80808080), "t3"}); when(NE, "t3", "x0", bytes);
        }
        o.ex(ADD, {"idx", c(1), "idx"}); o.ex(ADD, {"u", c(1), "u"}); o.ex(SUB, {"ii", c(4), "ii"}); o.jump(wl);
        o.L(bytes);                                          // from the word the loop stopped at, or the start
        if (true) { string al = o.fresh(); o.ex(OR, {"t", "t2", "t3"}); o.ex(AND, {"t3", c(3), "t3"}); when(NE, "t3", "x0", al);
            o.ex(SHL, {"idx", c(2), "t"}); o.ex(SHL, {"u", c(2), "t2"}); o.L(al); }
        o.L(bl);
        if (mc) when(EQ, "ii", "x0", same); else when(EQ, "ii", "x0", orig);
        o.ex(GETB, {"mem", "t", "bt"}); o.ex(BTOI, {"bt", "w"}); o.ex(GETB, {"mem", "t2", "bt"}); o.ex(BTOI, {"bt", "t3"});
        when(NE, "w", "t3", diff);
        if (!mc) when(EQ, "w", "x0", diff);
        o.ex(ADD, {"t", c(1), "t"}); o.ex(ADD, {"t2", c(1), "t2"}); o.ex(SUB, {"ii", c(1), "ii"}); o.jump(bl);
        o.L(same); o.copy("x0", "x10"); o.jump(fin);
        o.L(diff); o.ex(SUB, {"w", "t3", "x10"});
        o.L(fin);
        o.ex(SUB, {"sh", "ii", "sh"}); o.ex(ADD, {"sh", c(8), "sh"}); o.ex(SUB, {"gleft", "sh", "gleft"});
        nat_ret(o, pc);
        o.L(orig);
        return;
    }
    // 64-bit division: a1:a0 by a3:a2 into a1:a0
    bool uns = kind == N_UDIV || kind == N_UMOD, rem = kind == N_UMOD || kind == N_SMOD;
    o.ex(OR, {"x12", "x13", "t"}); when(EQ, "t", "x0", orig);
    if (uns) { when(LT, "x11", "x0", orig); when(LT, "x13", "x0", orig); }
    else { o.ex(AND, {"x12", "x13", "t"}); when(EQ, "t", c(MASK), orig); }      // -1: MIN / -1 throws
    o.ex(I2L, {"x10", "l"}); o.ex(I64("LogicalAnd"), {"l", "k_u32", "l"}); o.ex(I2L, {"x11", "l3"}); o.ex(I64SH("LeftShift"), {"l3", c(32), "l3"});
    o.ex(I64("LogicalOr"), {"l", "l3", "l"});
    o.ex(I2L, {"x12", "l2"}); o.ex(I64("LogicalAnd"), {"l2", "k_u32", "l2"}); o.ex(I2L, {"x13", "l3"}); o.ex(I64SH("LeftShift"), {"l3", c(32), "l3"});
    o.ex(I64("LogicalOr"), {"l2", "l3", "l2"});
    if (rem) { o.ex(I64("Division"), {"l", "l2", "l3"}); o.ex(I64("Multiplication"), {"l3", "l2", "l3"}); o.ex(I64("Subtraction"), {"l", "l3", "l"}); }
    else o.ex(I64("Division"), {"l", "l2", "l"});
    o.ex(I64SH("RightShift"), {"l", c(32), "l3"}); o.ex(L2I, {"l3", "x11"});
    o.ex(I64SH("LeftShift"), {"l", c(32), "l"}); o.ex(I64SH("RightShift"), {"l", c(32), "l"}); o.ex(L2I, {"l", "x10"});
    o.ex(SUB, {"gleft", c(60), "gleft"});
    nat_ret(o, pc);
    o.L(orig);
}
