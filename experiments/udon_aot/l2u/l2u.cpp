// The no-MMU Linux kernel translated to Udon bytecode at build time (a C++ port of linux2udon.py).
// l2u linux_payload.bin OUT_DIR [rootfs.bin PROGRAM...]: the same files as the Python.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <deque>
#include <map>
#include <functional>
#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <filesystem>

using namespace std;
typedef int64_t i64;
typedef uint32_t u32;

static const i64 RAM = 0x80000000LL, MIN = 0x80000000LL, MASK = 0xFFFFFFFFLL;
static const i64 RAMSIZE = 0x12000000LL, ROM = 0x40000000LL;
static const i64 RTC0 = 0x12000020LL, RTC1 = 0x26101901LL;
static const i64 TEXT = 0x82000000LL, TEND = 0x822d63ecLL;
static bool COUNT = true;
static bool FLT = true;                  // translate the F extension (L2U_FLOAT=0: the interpreter's)

static string I32(const char* n) { return string("SystemInt32.__op_") + n + "__SystemInt32_SystemInt32__SystemInt32"; }
static string CMP(const char* n) { return string("SystemInt32.__op_") + n + "__SystemInt32_SystemInt32__SystemBoolean"; }
static string I64(const char* n) { return string("SystemInt64.__op_") + n + "__SystemInt64_SystemInt64__SystemInt64"; }
static string I64SH(const char* n) { return string("SystemInt64.__op_") + n + "__SystemInt64_SystemInt32__SystemInt64"; }
static const string I64EQ = "SystemInt64.__op_Equality__SystemInt64_SystemInt64__SystemBoolean";
static const string AGET = "SystemInt32Array.__Get__SystemInt32__SystemInt32";
static const string ASET = "SystemInt32Array.__Set__SystemInt32_SystemInt32__SystemVoid";
static const string UGET = "SystemUInt32Array.__Get__SystemInt32__SystemUInt32";
static const string B2I = "SystemConvert.__ToInt32__SystemBoolean__SystemInt32";
static const string L2I = "SystemConvert.__ToInt32__SystemInt64__SystemInt32";
static const string I2L = "SystemConvert.__ToInt64__SystemInt32__SystemInt64";
static const string BIGMUL = "SystemMath.__BigMul__SystemInt32_SystemInt32__SystemInt64";
static const string CLAMP = "UnityEngineMathf.__Clamp__SystemInt32_SystemInt32_SystemInt32__SystemInt32";
static const string ACOPY = "SystemArray.__Copy__SystemArray_SystemInt32_SystemArray_SystemInt32_SystemInt32__SystemVoid";
static const string ACLEAR = "SystemArray.__Clear__SystemArray_SystemInt32_SystemInt32__SystemVoid";
static const string ADD = I32("Addition"), SUB = I32("Subtraction"), AND = I32("LogicalAnd"), OR = I32("LogicalOr"), XOR = I32("LogicalXor");
static const string SHL = I32("LeftShift"), SHR = I32("RightShift"), MUL = I32("Multiplication");
static const string EQ = CMP("Equality"), NE = CMP("Inequality"), LT = CMP("LessThan"), GT = CMP("GreaterThan"),
                    LE = CMP("LessThanOrEqual"), GE = CMP("GreaterThanOrEqual");
enum { T_I, T_U, T_B, T_L, T_S, T_IA, T_UA, T_BY, T_F, T_BA = 13 };
static string FOP(const char* n) { return string("SystemSingle.__op_") + n + "__SystemSingle_SystemSingle__SystemSingle"; }
static string FCMP(const char* n) { return string("SystemSingle.__op_") + n + "__SystemSingle_SystemSingle__SystemBoolean"; }
static const string FNEG = "SystemSingle.__op_UnaryMinus__SystemSingle__SystemSingle";
static const string BITS2F = "SystemBitConverter.__Int32BitsToSingle__SystemInt32__SystemSingle";
static const string F2BITS = "SystemBitConverter.__SingleToInt32Bits__SystemSingle__SystemInt32";
static string FMATH(const char* n) { return string("UnityEngineMathf.__") + n + "__SystemSingle__SystemSingle"; }
static const string I2F = "SystemConvert.__ToSingle__SystemInt32__SystemSingle";
static const string L2F = "SystemConvert.__ToSingle__SystemInt64__SystemSingle";
static const string F2I = "SystemConvert.__ToInt32__SystemSingle__SystemInt32";
static const string GETB = "SystemBuffer.__GetByte__SystemArray_SystemInt32__SystemByte";
static const string SETB = "SystemBuffer.__SetByte__SystemArray_SystemInt32_SystemByte__SystemVoid";
static const string BLOCKCOPY = "SystemBuffer.__BlockCopy__SystemArray_SystemInt32_SystemArray_SystemInt32_SystemInt32__SystemVoid";
static const string TOB = "SystemConvert.__ToByte__SystemInt32__SystemByte";
static const string BTOI = "SystemConvert.__ToInt32__SystemByte__SystemInt32";

static i64 sx(i64 v, int bits) { v &= (1LL << bits) - 1; return (v >> (bits - 1)) ? v - (1LL << bits) : v; }
static string fmt(const char* f, ...) { char b[256]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return b; }

struct Asm {
    vector<uint8_t> code;
    unordered_map<string, u32> labels;
    vector<pair<size_t, string>> fix, hfix_;
    unordered_map<string, int> slots;
    vector<string> slotOrder;
    vector<pair<int, i64>> heap;
    vector<string> externs;
    vector<pair<int, string>> hfix;
    int n = 0;
    vector<function<void()>> later;
    unordered_set<string> ext_wanted, istubs, hstubs;
    vector<u32> pcpos;
    unordered_set<i64> leaders;
    unordered_map<int, i64> boff; unordered_set<int> bhave;

    void w32(u32 v) { code.push_back(v >> 24); code.push_back(v >> 16); code.push_back(v >> 8); code.push_back(v); }
    string var(const string& name, int typ, i64 val = 0) {
        if (slots.count(name)) { fprintf(stderr, "dup var %s\n", name.c_str()); exit(1); }
        slots[name] = (int)heap.size(); slotOrder.push_back(name); heap.push_back({typ, val}); return name;
    }
    int sl(const string& name) {
        auto it = slots.find(name);
        if (it != slots.end()) return it->second;
        int r = (int)heap.size();
        if (name.size() < 2 || name[0] != '#') { fprintf(stderr, "unknown var %s\n", name.c_str()); exit(1); }
        if (name[1] == 'i') heap.push_back({T_I, sx((i64)strtoull(name.c_str() + 2, 0, 16), 32)});
        else if (name[1] == 'u') { heap.push_back({T_U, 0}); hfix.push_back({r, name.substr(2)}); }
        else if (name[1] == 'e') { heap.push_back({T_S, (i64)externs.size()}); externs.push_back(name.substr(2)); }
        else { fprintf(stderr, "bad var %s\n", name.c_str()); exit(1); }
        slots[name] = r; slotOrder.push_back(name);
        return r;
    }
    string c(i64 v) { char b[16]; snprintf(b, sizeof b, "#i%08x", (u32)(v & MASK)); return b; }
    string u(const string& label) { return "#u" + label; }
    string fresh() { n++; return "_" + to_string(n); }
    void L(const string& name) {
        if (labels.count(name)) { fprintf(stderr, "dup label %s\n", name.c_str()); exit(1); }
        labels[name] = (u32)code.size(); last = name;
    }
    string last;
    void push(const string& a) { int s = sl(a); w32(1); w32(s); }
    void ex(const string& sig, initializer_list<string> args) {
        for (auto& a : args) { int s = sl(a); w32(1); w32(s); }
        int e = sl("#e" + sig); w32(6); w32(e);
    }
    void copy(const string& a, const string& b) { int sa = sl(a); int sb = sl(b); w32(1); w32(sa); w32(1); w32(sb); w32(9); }
    void jump(const string& label) { fix.push_back({code.size() + 4, label}); w32(5); w32(0); }
    void jif(const string& cnd, const string& label) { int s = sl(cnd); w32(1); w32(s); fix.push_back({code.size() + 4, label}); w32(4); w32(0); }
    void jind(const string& a) { int s = sl(a); w32(8); w32(s); }
    void exit_() { w32(5); w32(0xFFFFFFFC); }
    void finish() {
        for (auto& f : fix) {
            auto it = labels.find(f.second);
            if (it == labels.end()) { fprintf(stderr, "no label %s\n", f.second.c_str()); exit(1); }
            u32 v = it->second; code[f.first] = v >> 24; code[f.first + 1] = v >> 16; code[f.first + 2] = v >> 8; code[f.first + 3] = v;
        }
        for (auto& h : hfix) heap[h.first].second = labels.at(h.second);
    }
};

static string x(int r) { return r == 32 ? "da" : r == 33 ? "db" : "x" + to_string(r); }   // 32, 33: the dynamic tier's staged sources

// ---------------------------------------------------------------- shared pieces

static void emit_mul(Asm& o, int f3, const string& a, const string& b, const string& rd) {
    if (f3 == 0) { o.ex(MUL, {a, b, rd}); return; }
    if (f3 <= 3) {
        o.ex(BIGMUL, {a, b, "l"}); o.ex(I64SH("RightShift"), {"l", o.c(32), "l"}); o.ex(L2I, {"l", "t3"});
        if (f3 == 3) { o.ex(SHR, {a, o.c(31), "t"}); o.ex(AND, {"t", b, "t"}); o.ex(ADD, {"t3", "t", "t3"}); }
        if (f3 == 2 || f3 == 3) { o.ex(SHR, {b, o.c(31), "t"}); o.ex(AND, {"t", a, "t"}); o.ex(ADD, {"t3", "t", "t3"}); }
        o.copy("t3", rd); return;
    }
    o.ex(I2L, {a, "l"}); o.ex(I2L, {b, "l2"});
    if (f3 == 5 || f3 == 7) { o.ex(I64("LogicalAnd"), {"l", "k_u32", "l"}); o.ex(I64("LogicalAnd"), {"l2", "k_u32", "l2"}); }
    string nz = o.fresh(), done = o.fresh();
    o.ex(I64EQ, {"l2", "k_l0", "cnd"}); o.jif("cnd", nz);
    o.copy(f3 < 6 ? o.c(MASK) : a, "t3"); o.jump(done);
    o.L(nz);
    if (f3 < 6) o.ex(I64("Division"), {"l", "l2", "l"});
    else {
        o.ex(I64("Division"), {"l", "l2", "l3"}); o.ex(I64("Multiplication"), {"l3", "l2", "l3"});
        o.ex(I64("Subtraction"), {"l", "l3", "l"});
    }
    o.ex(I64SH("LeftShift"), {"l", o.c(32), "l"}); o.ex(I64SH("RightShift"), {"l", o.c(32), "l"});
    o.ex(L2I, {"l", "t3"});
    o.L(done);
    o.copy("t3", rd);
}

static void emit_srl(Asm& o, const string& a, const string& b, const string& rd) {
    o.ex(SHR, {o.c(MIN), b, "t"}); o.ex(SHL, {"t", o.c(1), "t"}); o.ex(XOR, {"t", o.c(MASK), "t"});
    o.ex(SHR, {a, b, "t2"}); o.ex(AND, {"t2", "t", rd});
}

static void emit_insert(Asm& o, int width, const string& v) {
    o.ex(SHL, {"t", o.c(3), "sh"});                    // a shift takes its count modulo 32: 8 * (t & 3)
    o.ex(SHL, {o.c((1 << width) - 1), "sh", "t2"}); o.ex(XOR, {"t2", o.c(MASK), "t2"});
    o.ex(AND, {"w", "t2", "w"});
    o.ex(AND, {v, o.c((1 << width) - 1), "t2"}); o.ex(SHL, {"t2", "sh", "t2"}); o.ex(OR, {"w", "t2", "w"});
    o.ex(ASET, {"mem", "idx", "w"});
}

static void emit_extract(Asm& o, int f3, const string& rd) {
    int width = (f3 == 0 || f3 == 4) ? 8 : 16;
    o.ex(SHL, {"t", o.c(3), "sh"});
    if (f3 == 4 || f3 == 5) { o.ex(SHR, {"w", "sh", "w"}); o.ex(AND, {"w", o.c((1 << width) - 1), rd}); }
    else { o.ex(SUB, {o.c(32 - width), "sh", "sh"}); o.ex(SHL, {"w", "sh", "w"}); o.ex(SHR, {"w", o.c(32 - width), rd}); }
}

static bool DIRTY = false;              // L2U_DIRTY=0 turns off: every write of mem lists its 4 KB page (pdirty, plist, pn)
static bool DIRTYB = false;             // L2U_DIRTY=2: pdirty a bool[] (one extern a mark fewer)
static const string BGET = "SystemBooleanArray.__Get__SystemInt32__SystemBoolean";
static const string BSET = "SystemBooleanArray.__Set__SystemInt32_SystemBoolean__SystemVoid";
static const int NPAGES = 73728;       // RAM 0x80000000-0x92000000 in pages, word index >> 10

// pg (a variable or constant page) written: out of line when it is new, inline (cold sites) when !out
static bool MFAST = true;              // the inline check; else a call of markchk (smaller, for stores run less)
// pair: page pg and the next both, known by pdirty[pg] == 2 (all set entries are cleared together)
static int PAIR = 2;                  // L2U_PAIR: groups that check two pages at once: 0 none, 1 all, 2 the stack's (the page above is the callers')
static void mark_pg(Asm& o, const string& pg, bool out = true, bool pair = false) {
    if (out && !MFAST) {
        string back = o.fresh();
        if (pg != "pg") o.copy(pg, "pg");
        o.copy(o.u(back), "dret"); o.jump(pair ? "markchk2" : "markchk"); o.L(back);
        return;
    }
    if (DIRTYB) o.ex(BGET, {"pdirty", pg, "cnd"}); else o.ex(AGET, {"pdirty", pg, "pd"});
    if (out) {
        string stub = o.fresh(), back = o.fresh();
        if (pair) o.ex(EQ, {"pd", o.c(2), "cnd"}); else if (!DIRTYB) o.ex(NE, {"pd", "x0", "cnd"});
        o.jif("cnd", stub); o.L(back);
        o.later.push_back([&o, stub, back, pg, pair]() { o.L(stub); if (pg != "pg") o.copy(pg, "pg"); o.copy(o.u(back), "dret"); o.jump(pair ? "markpg2" : "markpg"); });
        return;
    }
    string skip = o.fresh(), mk = o.fresh();
    if (DIRTYB) { o.jif("cnd", mk); o.jump(skip); o.L(mk); o.ex(BSET, {"pdirty", pg, "k_true"}); }
    else { o.ex(EQ, {"pd", "x0", "cnd"}); o.jif("cnd", skip); o.ex(ASET, {"pdirty", pg, o.c(1)}); }
    o.ex(ASET, {"plist", "pn", pg}); o.ex(ADD, {"pn", o.c(1), "pn"});
    o.L(skip);
}
static void mark_word(Asm& o, const string& idx, bool out = true, bool pair = false) { o.ex(SHR, {idx, o.c(10), "pg"}); mark_pg(o, "pg", out, pair); }
static void mark_byte(Asm& o, const string& off, bool out = true, bool pair = false) { o.ex(SHR, {off, o.c(12), "pg"}); mark_pg(o, "pg", out, pair); }
// the pages of mem offsets lo..hi (bytes, inclusive; variables), then back to label `back`
static void mark_range(Asm& o, const string& lo, const string& hi) {
    string back = o.fresh();
    if (lo != "mlo") o.copy(lo, "mlo");
    if (hi != "mhi") o.copy(hi, "mhi");
    o.copy(o.u(back), "dret2"); o.jump("markrange"); o.L(back);
}
static void dirty_routines(Asm& o) {
    o.L("markpg"); if (DIRTYB) o.ex(BSET, {"pdirty", "pg", "k_true"}); else o.ex(ASET, {"pdirty", "pg", o.c(1)}); o.ex(ASET, {"plist", "pn", "pg"}); o.ex(ADD, {"pn", o.c(1), "pn"}); o.jind("dret");
    o.L("markchk");
    if (DIRTYB) o.ex(BGET, {"pdirty", "pg", "cnd"}); else { o.ex(AGET, {"pdirty", "pg", "pd"}); o.ex(NE, {"pd", "x0", "cnd"}); }
    o.jif("cnd", "markpg"); o.jind("dret");
    if (!DIRTYB) {
        o.L("markchk2"); o.ex(AGET, {"pdirty", "pg", "pd"}); o.ex(EQ, {"pd", o.c(2), "cnd"}); o.jif("cnd", "markpg2"); o.jind("dret");
        o.L("markpg2"); o.ex(AGET, {"pdirty", "pg", "pd"}); o.ex(EQ, {"pd", "x0", "cnd"}); o.jif("cnd", "mp2_set");
        o.ex(ASET, {"plist", "pn", "pg"}); o.ex(ADD, {"pn", o.c(1), "pn"});
        o.L("mp2_set"); o.ex(ASET, {"pdirty", "pg", o.c(2)}); o.ex(ADD, {"pg", o.c(1), "pg"});
        o.ex(LT, {"pg", o.c(NPAGES), "cnd"}); o.jif("cnd", "mp2_done");
        o.ex(AGET, {"pdirty", "pg", "pd"}); o.ex(NE, {"pd", "x0", "cnd"}); o.jif("cnd", "markpg");
        o.L("mp2_done"); o.jind("dret");
    }
    o.L("markrange"); o.ex(SHR, {"mlo", o.c(12), "mp"}); o.ex(SHR, {"mhi", o.c(12), "mq"});
    o.L("mr_loop"); o.ex(GT, {"mp", "mq", "cnd"}); o.jif("cnd", "mr_body"); o.jind("dret2");
    o.L("mr_body"); mark_pg(o, "mp", false); o.ex(ADD, {"mp", o.c(1), "mp"}); o.jump("mr_loop");
}

static void fsync_out(Asm& o);
static void fsync_in(Asm& o);
static void sync_to_xr(Asm& o) { for (int r = 1; r < 32; r++) o.ex(ASET, {"xr", o.c(r), x(r)}); if (FLT) fsync_out(o); }
static void sync_from_xr(Asm& o) { for (int r = 1; r < 32; r++) o.ex(AGET, {"xr", o.c(r), x(r)}); if (FLT) fsync_in(o); }

// ---------------------------------------------------------------- the translated text

enum K { K_LUI, K_AUIPC, K_JAL, K_JALR, K_BRANCH, K_LOAD, K_STORE, K_OPIMM, K_OP, K_FENCE, K_AMO, K_FLOAD, K_FSTORE, K_FP, K_FMA, K_CSR, K_INTERP };
static bool fp_ok(u32 w);
static bool csr_ok(u32 w);
static bool CSRT = true;                 // translate CSR instructions (L2U_CSR=0: the interpreter's)
struct D { u32 w; int op, rd, f3, rs1, rs2, f7, f5; K k; i64 imm; };

static D decode(u32 w) {
    D d{}; d.w = w; d.op = w & 0x7F; d.rd = (w >> 7) & 31; d.f3 = (w >> 12) & 7; d.rs1 = (w >> 15) & 31; d.rs2 = (w >> 20) & 31; d.f7 = w >> 25;
    i64 i_imm = sx(w >> 20, 12);
    int op = d.op;
    if (op == 0x37) { d.k = K_LUI; d.imm = w & 0xFFFFF000u; }
    else if (op == 0x17) { d.k = K_AUIPC; d.imm = w & 0xFFFFF000u; }
    else if (op == 0x6F) { d.k = K_JAL; d.imm = sx(((i64)(w >> 31) << 20) | (((w >> 12) & 255) << 12) | (((w >> 20) & 1) << 11) | (((w >> 21) & 1023) << 1), 21); }
    else if (op == 0x67 && d.f3 == 0) { d.k = K_JALR; d.imm = i_imm; }
    else if (op == 0x63 && d.f3 != 2 && d.f3 != 3) { d.k = K_BRANCH; d.imm = sx(((i64)(w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 63) << 5) | (((w >> 8) & 15) << 1), 13); }
    else if (op == 0x03 && (d.f3 == 0 || d.f3 == 1 || d.f3 == 2 || d.f3 == 4 || d.f3 == 5)) { d.k = K_LOAD; d.imm = i_imm; }
    else if (op == 0x23 && d.f3 <= 2) { d.k = K_STORE; d.imm = sx(((w >> 25) << 5) | ((w >> 7) & 31), 12); }
    else if (op == 0x13) { d.k = K_OPIMM; d.imm = i_imm; }
    else if (op == 0x33 && (d.f7 == 0 || d.f7 == 1 || d.f7 == 0x20) && (d.f7 != 0x20 || d.f3 == 0 || d.f3 == 5)) d.k = K_OP;
    else if (op == 0x0F) d.k = K_FENCE;
    else if (op == 0x2F && d.f3 == 2) {
        int f5 = w >> 27;
        if (f5 == 0 || f5 == 1 || f5 == 2 || f5 == 3 || f5 == 4 || f5 == 8 || f5 == 12 || f5 == 16 || f5 == 20 || f5 == 24 || f5 == 28) { d.k = K_AMO; d.f5 = f5; }
        else d.k = K_INTERP;
    }
    else if (FLT && op == 0x07 && d.f3 == 2) { d.k = K_FLOAD; d.imm = i_imm; }
    else if (FLT && op == 0x27 && d.f3 == 2) { d.k = K_FSTORE; d.imm = sx(((w >> 25) << 5) | ((w >> 7) & 31), 12); }
    else if (FLT && op == 0x53 && fp_ok(w)) d.k = K_FP;
    else if (FLT && (op == 0x43 || op == 0x47 || op == 0x4B || op == 0x4F) && ((w >> 25) & 3) == 0) d.k = K_FMA;
    else if (CSRT && op == 0x73 && csr_ok(w)) d.k = K_CSR;
    else d.k = K_INTERP;
    return d;
}

struct Seg {
    i64 lo, hi, bss = 0; bool user; int sid; string pre, jt, tk, tim, name;
    vector<u32> words; unordered_set<i64> data; i64 entry = 0;
    vector<pair<i64, u32>> sig;
    vector<int> tkv; vector<u32> jtv, pcpos; unordered_set<i64> lead; vector<string> tlab; vector<int32_t> timv; i64 pbase = 0;
    Seg(i64 lo_, i64 hi_, bool user_ = false, int sid_ = 0) : lo(lo_), hi(hi_), user(user_), sid(sid_) {
        pre = user ? "s" + to_string(sid) + "_" : "";
        jt = user ? "jt_s" + to_string(sid) : "jt"; tk = user ? "tk_s" + to_string(sid) : "tk"; tim = user ? "tim_s" + to_string(sid) : "tim";
    }
    string lab(const char* kind, i64 v) const { char b[32]; snprintf(b, sizeof b, "%llx", (unsigned long long)v); return pre + kind + b; }
};

static Seg* S = nullptr;
static int TIER = 0;                     // 0: all inline; 1: inline blocks, a routine a word between them; 2: inline only where hot
static bool DYN = true;                  // L2U_DYN=0: untranslated programs are the interpreter's, not the dynamic tier's
static unordered_set<i64> INL;           // the current segment's leaders emitted inline (TIER > 0)
static bool in_text(i64 pc) { return S->lo <= pc && pc < S->hi; }

static void pcv(Asm& o, const Seg* seg, i64 v, const string& dest) {
    if (seg->user) o.ex(ADD, {"ud", o.c(v), dest}); else o.copy(o.c(v), dest);
}

static string target(Asm& o, i64 t) {
    if (in_text(t) && t % 4 == 0 && TIER && !INL.count(t)) {       // into the threaded routines at t
        string lab = S->lab("te", t);
        if (!o.istubs.count(lab)) {
            o.istubs.insert(lab); Seg* seg = S;
            o.later.push_back([&o, lab, t, seg]() { o.L(lab); o.copy(o.c((t - seg->lo + 4) / 4), "tp"); o.jump("tdent"); });
        }
        return lab;
    }
    if (in_text(t) && t % 4 == 0) return S->lab("p", t);
    string lab = S->lab("ext", t);
    if (!o.labels.count(lab) && !o.ext_wanted.count(lab)) {
        o.ext_wanted.insert(lab);
        Seg* seg = S;
        o.later.push_back([&o, lab, t, seg]() { o.L(lab); pcv(o, seg, t, "ipc"); o.jump("to_interp"); });
    }
    return lab;
}

static void t_halt(Asm& o, i64 pc, const Seg* seg = nullptr) {
    if (!seg) seg = S;
    bool inl = !TIER || (seg == S && INL.count(pc));
    o.copy(o.u(inl ? seg->lab("p", pc) : seg->pre + "resume_rpc"), "resume"); pcv(o, seg, pc, "rpc"); o.jump("exit");
}

static bool ISTAT = false;               // L2U_ISTAT=1: the interpreter counts opcodes, pairs and where it runs
static bool PROF = false;                 // count each run of an interpreter stub in prof[] (L2U_PROF=1)
static vector<pair<int, i64>> prof_sites;
static void prof_count(Asm& o, const Seg* seg, i64 pc) {
    if (!PROF) return;
    string id = o.c((i64)prof_sites.size()); prof_sites.push_back({seg->user ? seg->sid : -1, pc});
    o.ex(AGET, {"prof", id, "pt"}); o.ex(ADD, {"pt", o.c(1), "pt"}); o.ex(ASET, {"prof", id, "pt"});
}

static void rom_word(Asm& o, const string& no) {      // w = the ROM word at address t; t not in the ROM: to no
    o.ex(SUB, {"t", o.c(ROM), "t2"}); o.ex(GE, {"t2", "x0", "cnd"}); o.jif("cnd", no);
    o.ex(SHR, {"t2", o.c(2), "t3"}); o.ex(LT, {"t3", "romlen", "cnd"}); o.jif("cnd", no);
    o.ex(AGET, {"rom", "t3", "w"});
}

static string to_interp_at(Asm& o, i64 pc) {
    string lab = S->lab("i", pc);
    if (!o.istubs.count(lab)) {
        o.istubs.insert(lab);
        Seg* seg = S;
        o.later.push_back([&o, lab, pc, seg]() { o.L(lab); prof_count(o, seg, pc); pcv(o, seg, pc, "ipc"); o.jump("to_interp"); });
    }
    return lab;
}

static string thr_at(Asm& o, i64 pc);
#include "sec_emit.inc"

static void t_opimm(Asm& o, Item& it) {
    D& d = it.d; string rd = x(d.rd), rs1 = it.A; i64 imm = d.imm; int f3 = d.f3; int sh = imm & 31;
    if (f3 == 0) {
        if (imm == 0) o.copy(rs1, rd);
        else if (d.rs1 == 0) o.copy(o.c(imm), rd);
        else o.ex(ADD, {rs1, o.c(imm), rd});
    } else if (f3 == 1) o.ex(SHL, {rs1, o.c(sh), rd});
    else if (f3 == 5) {
        if (d.w >> 30 & 1) o.ex(SHR, {rs1, o.c(sh), rd});
        else if (sh == 0) o.copy(rs1, rd);
        else { o.ex(SHR, {rs1, o.c(sh), "t"}); o.ex(AND, {"t", o.c(MASK >> sh), rd}); }
    } else if (f3 == 2) { o.ex(LT, {rs1, o.c(imm), "cnd"}); o.ex(B2I, {"cnd", rd}); }
    else if (f3 == 3) {
        if (imm == 1) o.ex(EQ, {rs1, "x0", "cnd"});
        else { o.ex(XOR, {rs1, o.c(MIN), "t"}); o.ex(LT, {"t", o.c((imm & MASK) ^ MIN), "cnd"}); }
        o.ex(B2I, {"cnd", rd});
    } else o.ex(f3 == 4 ? XOR : f3 == 6 ? OR : AND, {rs1, o.c(imm), rd});
}

static void t_op(Asm& o, Item& it) {
    D& d = it.d; string rd = x(d.rd), a = it.A, b = it.B; int f3 = d.f3, f7 = d.f7;
    if (f3 == 0) o.ex(f7 == 0x20 ? SUB : ADD, {a, b, rd});
    else if (f3 == 1) o.ex(SHL, {a, b, rd});
    else if (f3 == 2) { o.ex(LT, {a, b, "cnd"}); o.ex(B2I, {"cnd", rd}); }
    else if (f3 == 3) {
        if (d.rs1 == 0) o.ex(NE, {b, "x0", "cnd"});
        else { o.ex(XOR, {a, o.c(MIN), "t"}); o.ex(XOR, {b, o.c(MIN), "t2"}); o.ex(LT, {"t", "t2", "cnd"}); }
        o.ex(B2I, {"cnd", rd});
    } else if (f3 == 4) o.ex(XOR, {a, b, rd});
    else if (f3 == 5) { if (f7 == 0x20) o.ex(SHR, {a, b, rd}); else emit_srl(o, a, b, rd); }
    else if (f3 == 6) o.ex(OR, {a, b, rd});
    else o.ex(AND, {a, b, rd});
}

static void amo_value(Asm& o, int f5, const string& v) {
    if (f5 == 1) { o.copy(v, "t3"); return; }
    if (f5 == 0 || f5 == 4 || f5 == 8 || f5 == 12) { o.ex(f5 == 0 ? ADD : f5 == 4 ? XOR : f5 == 8 ? OR : AND, {"w", v, "t3"}); return; }
    string keep = o.fresh();
    string a = "w", b = v;
    if (f5 >= 24) { o.ex(XOR, {"w", o.c(MIN), "t"}); o.ex(XOR, {v, o.c(MIN), "t2"}); a = "t"; b = "t2"; }
    o.copy("w", "t3");
    o.ex((f5 == 16 || f5 == 24) ? LT : GT, {a, b, "cnd"}); o.jif("cnd", keep);
    o.jump(keep + "k");
    o.L(keep); o.copy(v, "t3");
    o.L(keep + "k");
}

static void t_amo(Asm& o, Item& it) {
    D& d = it.d; int f5 = d.f5, rd = d.rd, rs1 = d.rs1; string v = it.B;
    o.ex(ADD, {x(rs1), o.c(MIN), "t"}); o.ex(SHR, {"t", o.c(2), "idx"});
    if (f5 == 2) { o.copy(x(rs1), "res"); if (rd) o.ex(AGET, {"mem", "idx", x(rd)}); return; }
    if (f5 == 3) {
        string fail = o.fresh(), done = o.fresh();
        o.ex(EQ, {"res", x(rs1), "cnd"}); o.copy(o.c(MASK), "res"); o.jif("cnd", fail);
        if (DIRTY) mark_word(o, "idx");
        o.ex(ASET, {"mem", "idx", v});
        if (rd) o.copy("x0", x(rd));
        o.jump(done); o.L(fail);
        if (rd) o.copy(o.c(1), x(rd));
        o.L(done); return;
    }
    o.ex(AGET, {"mem", "idx", "w"});
    amo_value(o, f5, v);
    if (DIRTY) mark_word(o, "idx");
    o.ex(ASET, {"mem", "idx", "t3"});
    if (rd) o.copy("w", x(rd));
}

#include "sec_insn.inc"
