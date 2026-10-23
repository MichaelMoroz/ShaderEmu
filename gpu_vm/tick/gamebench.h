// The game bench (gpu_vm/tick/GAMEBENCH.md): game-like kernels in gpu_vm assembly, as gvmbench cases `g_*` (the set
// `game`: gvmbench --set game). A kernel is a function of a few parameters written with gvm_asm.h's op-level calls only
// (the assembler bundles them), so a step with another encoding runs the same work. The same kernels in C for the
// RISC-V machine: gpu_vm/tick/gamebench_rv/gamebench.c (its numbers in g_rv_of below).
//
//   g_logic   core 0 alone: a game's frame of entity logic (Half-Life's core 0 as measured: calls through a table,
//             stack frames four deep with a trace's result struct in them, struct copies, a hash table and heap nodes,
//             a BSP-like trace, some floats)
//   g_verts   one core: a model's vertices transformed by bones (a worker's float loop over arrays)
//   g_ray1    one core: rays against spheres (nxray's worker)
//   g_q3      core 0 + 3 busy workers (a server's logic, a scene's vertices, a client helper) + 2 awake idle
//   g_hl      core 0 (the heavy logic) + 4 busy workers + 40 awake idle
//   g_ray     core 0 + 191 workers sharing a picture in chunks of 32 pixels
// and the same work in other forms (the set game_x):
//   ..._r     the stack (frames' saves, spills, the trace's struct) in registers: moves (a large register file)
//   ..._f     the stack in the core's file (lf, sf: phase 2; skipped on a tick without a file of 128 words)
//   ..._c     the snapshots' struct copies by the commit's copy operation (GVM_OPS) instead of loads and stores
//
// The scenes hand jobs to workers as programs/mc/mcw.c does: a job texel {seq, kind, a0, a1} a core, an answer texel
// {done, result}; a worker polls its job word and ends its pass by `sync` while it has none, after a few bundles that
// stand for the firmware's timer trap (gpu_vm/platform/worker.S). Core 0 posts frame f's jobs, runs its own logic,
// and waits (sync) for them at the start of frame f + 1, so its work and the workers' overlap.
// The score is frames a second (core 0's r60 counts entities, or chunks: a frame is unit_div of them).
#pragma once

// ---- Memory (the cases run with 128 MB of RAM: Case::big)
static const uint32_t G_DATA = 0x6000000;           // logic instance k: G_DATA + k * G_INST
static const uint32_t G_INST = 0x60000;             //   +0 entities (128 B each), +0x20000 snapshots, +0x40000 hash heads (1,024
static const uint32_t G_SNAP = 0x20000, G_HASH = 0x40000, G_TABLE = 0x41000, G_HEAP = 0x42000;   // words), +0x41000 the think table, +0x42000 heap nodes (16 B)
static const uint32_t G_WORLD = 0x6600000;          // 4,096 tree nodes {normal, dist, child 0, child 1}
static const uint32_t G_MBOX = 0x6610000, G_ANS = 0x6611000;   // a texel a core each (G_ANS = G_MBOX + 0x1000)
static const uint32_t G_SPHERES = 0x6612000;        // 32 B a sphere {cx, cy, cz, cc, color, -, -, -}
static const uint32_t G_CONST = 0x6613000;          // read-only constants, reached by lpc (a PIC program's .rodata)
static const uint32_t G_FB = 0x6620000;             // the ray picture, a word a pixel
static const uint32_t G_VERTS = 0x6700000, G_BONES = 0x6710000;   // 4,096 vertices {x, y, z, bone}, 32 bones of 64 B
static const uint32_t G_VOUT = 0x6800000;           // vertex output k at G_VOUT + k * 0x20000
static const uint32_t G_STACK = 0x7000000;          // core c's stack: 16 KB below G_STACK + (c + 1) * 0x4000

// Where the stack lives (the _r and _f cases): memory, registers, the core's file
enum { G_MEM = 0, G_REGS = 1, G_FILE = 2 };
struct GLogic {   // entities (a power of 2), traces a physics think, the tree's depth, spill pairs a think, a snapshot every
                  // `snap` entities, AI neighbours, where the stack is, snapshots by the copy operation
    int E = 0, T = 1, D = 6, spills = 2, snap = 4, ai = 4, stack = G_MEM;
    bool copyop = false;
};
struct GJob { int kind, size; };                      // 1 logic (size entities, instance = the worker), 2 vertices (size)
struct GRay { int w = 256, h = 192, s = 16, spill = 2, stack = G_MEM; };   // the picture, spheres, spill pairs a pixel
struct GScene {
    GLogic core0, worker;      // core 0's logic; the logic of the workers' kind 1 jobs (its E from the job)
    std::vector<GJob> jobs;    // busy worker k + 1's job each frame
    int idle = 0;              // awake idle workers after them
    int rayW = 0;              // > 0: workers 1..rayW share core 0's picture each frame
    GRay ray;
    int idleOps = 10;          // the idle poll's extra ops (worker.S's timer trap)
};

// Registers: r1 ra, r2 sp, r3 frames (core 0) or the last job (a worker), r4 frame, r5-r19 temporaries, r20-r27 saved,
// r28-r43 the frames' saves in registers (G_REGS: depth 1-4, four each), r44-r49 the logic's bases, r50-r53 the trace's
// struct and r54-r55 spills in registers (G_REGS), r56 a worker's job texel, r57 its kind, r58-r59 the idle poll's
// (time-dependent), r60 units, r61-r62 the special functions' sinks, r63 the file's stack index (G_FILE).
static const uint64_t G_SKIP_REGS = (1ull << 58) | (1ull << 59) | (1ull << 61) | (1ull << 62);

// Branches compare the registers as their bundle found them: a branch on a register its own bundle writes opens a new
// bundle first (as a compiler schedules it)
static void g_br(GvmAsm& a, uint32_t cc, uint32_t ra, uint32_t rb, int l) {
    bool imm = cc >= GVM_CC_BBS;   // (bbs, bbc and the immediate forms: rb is a number)
    if (a.writes(ra) || (!imm && a.writes(rb))) a.close();
    a.br(cc, ra, rb, l);
}
static void g_beq(GvmAsm& a, uint32_t x, uint32_t y, int l) { g_br(a, GVM_CC_BEQ, x, y, l); }
static void g_bne(GvmAsm& a, uint32_t x, uint32_t y, int l) { g_br(a, GVM_CC_BNE, x, y, l); }
static void g_bnei(GvmAsm& a, uint32_t x, int32_t i, int l) { g_br(a, GVM_CC_BNEI, x, (uint32_t)i & 63, l); }

static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// A function's frame (depth 1-4) and the stack's other traffic, by where the stack lives. Memory: 64 bytes a frame, the
// saves at 0-15, spills at 16-31, a callee's result struct at 32-63 (r9 points the callee at it). Registers: banks of
// moves. The file: 16 words a frame below r63, the same layout in words.
struct GEmit {
    GvmAsm& a;
    int m;
    GEmit(GvmAsm& as, int mode) : a(as), m(mode) {}
    void prologue(int depth, std::initializer_list<uint32_t> regs) {
        int i = 0;
        if (m == G_REGS) { for (uint32_t r : regs) a.mv(28 + 4 * (depth - 1) + i++, r); return; }
        if (m == G_FILE) { a.addi(63, 63, -16); for (uint32_t r : regs) a.sf(r, 63, i++); return; }
        a.addi(2, 2, -64);
        for (uint32_t r : regs) a.sw(r, 2, 4 * i++);
    }
    void epilogue(int depth, std::initializer_list<uint32_t> regs) {
        int i = 0;
        if (m == G_REGS) { for (uint32_t r : regs) a.mv(r, 28 + 4 * (depth - 1) + i++); return; }
        if (m == G_FILE) { for (uint32_t r : regs) a.lf(r, 63, i++); a.addi(63, 63, 16); return; }
        for (uint32_t r : regs) a.lw(r, 2, 4 * i++);
        a.addi(2, 2, 64);
    }
    void spill(int k, uint32_t r) { if (m == G_REGS) a.mv(54 + (k & 1), r); else if (m == G_FILE) a.sf(r, 63, 4 + (k & 3)); else a.sw(r, 2, 16 + 4 * (k & 3)); }
    void fill(int k, uint32_t r) { if (m == G_REGS) a.mv(r, 54 + (k & 1)); else if (m == G_FILE) a.lf(r, 63, 4 + (k & 3)); else a.lw(r, 2, 16 + 4 * (k & 3)); }
    void struct_at() { if (m == G_FILE) a.addi(9, 63, 8); else if (m == G_MEM) a.addi(9, 2, 32); }   // r9: the caller's struct
    void struct_st(int k, uint32_t r) { if (m == G_REGS) a.mv(50 + (k & 3), r); else if (m == G_FILE) a.sf(r, 9, k); else a.sw(r, 9, 4 * k); }
    void struct_ld(int k, uint32_t r) { if (m == G_REGS) a.mv(r, 50 + (k & 3)); else if (m == G_FILE) a.lf(r, 63, 8 + k); else a.lw(r, 2, 32 + 4 * k); }
};

// The logic's functions. logic_frame(r5 instance, r6 E, r7 frame): every entity through dispatch.
struct GLogicCode { int frame; };
static GLogicCode g_emit_logic(GvmAsm& a, const GLogic& P) {
    GEmit e(a, P.stack);
    int frame = a.label(), dispatch = a.label(), think[4] = {a.label(), a.label(), a.label(), a.label()};
    int pm_trace = a.label(), trace = a.label(), link = a.label(), memcpy_ = a.label();
    // ---- logic_frame
    a.bind(frame);
    e.prologue(1, {1, 20, 21, 26});
    a.mv(44, 5); a.addi(45, 5, (int32_t)G_SNAP); a.addi(46, 5, (int32_t)G_HASH); a.addi(47, 5, (int32_t)G_HEAP);
    a.li(48, (int32_t)G_WORLD); a.addi(49, 6, -1); a.mv(26, 6); a.mv(27, 7);
    a.addi(8, 5, (int32_t)G_TABLE);   // the think table, every frame (a game's function pointers)
    for (int k = 0; k < 4; k++) { a.lpc(9, think[k]); a.sw(9, 8, 4 * k); }
    a.li(20, 0); a.mv(21, 44);
    int el = a.here();
    a.call(dispatch);
    a.addi(20, 20, 1); a.addi(21, 21, 128); a.addi(60, 60, 1);
    g_bne(a, 20, 26, el);
    e.epilogue(1, {1, 20, 21, 26});
    a.li(5, 0);
    a.ret();
    // ---- dispatch(r20 e, r21 entity)
    a.bind(dispatch);
    {
        e.prologue(2, {1, 22, 23, 24});
        int skip = a.label(), nosnap = a.label();
        a.lw(5, 21, 0); a.lw(6, 21, 4);
        a.lpca(10, G_CONST + 28); a.lw(10, 10, 0); a.op(GVM_I_OR, 6, 6, 10);   // (a global: 0)
        g_br(a, GVM_CC_BBS, 6, 0, skip);                       // inactive
        a.addi(7, 46, (int32_t)(G_TABLE - G_HASH)); a.shadd(7, 7, 5, 2); a.lw(7, 7, 0);
        // the think's frame lower by 64 bytes times e & 3 (a game's call chains reach different depths): the stack's texels
        if (e.m == G_MEM) { a.opi(GVM_I_AND, 22, 20, 3); a.opi(GVM_I_SLL, 22, 22, 6); a.sub(2, 2, 22); }
        else if (e.m == G_FILE) { a.opi(GVM_I_AND, 22, 20, 3); a.opi(GVM_I_SLL, 22, 22, 4); a.sub(63, 63, 22); }
        a.callr(7);
        if (e.m == G_MEM) a.add(2, 2, 22);
        else if (e.m == G_FILE) a.add(63, 63, 22);
        a.call(link);
        a.add(8, 20, 27); a.opi(GVM_I_AND, 8, 8, P.snap - 1);
        g_bnei(a, 8, 0, nosnap);                                // one entity in snap: its state copied to the snapshots
        a.opi(GVM_I_SLL, 8, 20, 7);
        if (P.copyop) {   // by the commit's copy operation: {copy, dst, src, bytes} stored at GVM_OPS
            a.li(12, GVM_OP_COPY); a.add(13, 45, 8); a.mv(14, 21); a.li(15, 128); a.li(16, (int32_t)GVM_OPS);
            a.st(GVM_W_Q, 12, 16, 0);
        } else {
            a.add(5, 45, 8); a.mv(6, 21); a.li(7, 128);
            a.call(memcpy_);
        }
        a.bind(nosnap);
        a.bind(skip);
        e.epilogue(2, {1, 22, 23, 24});
        a.ret();
    }
    // ---- think 0 and 3: physics, and traces (3 twice as many) whose results come back in a struct on the stack
    for (int kind : {0, 3}) {
        a.bind(think[kind]);
        e.prologue(3, {1, 22, 23, 24});
        int l1 = a.label(), l2 = a.label(), l3 = a.label();
        a.lw(8, 21, 8); a.lw(9, 21, 12); a.lw(10, 21, 16);
        a.lw(11, 21, 20); a.lw(12, 21, 24); a.lw(13, 21, 28);
        a.lpca(14, G_CONST); a.lw(15, 14, 0); a.lw(16, 14, 4);   // dt, g * dt
        a.fop(GVM_F_MAD, 8, 11, 15, 8); a.fop(GVM_F_MAD, 9, 12, 15, 9); a.fop(GVM_F_MAD, 10, 13, 15, 10);
        a.fop(GVM_F_SUB, 13, 13, 16);
        a.fop(GVM_F_LT, 17, 10, 0);
        g_beq(a, 17, 0, l1);                                    // under the floor: on it, and up again
        a.li(10, 0); a.lw(13, 14, 8);
        a.bind(l1);
        a.fopi(GVM_F_LT, 17, 8, 512.0f);
        g_bnei(a, 17, 0, l2);                                   // past the edge: around
        a.fopi(GVM_F_SUB, 8, 8, 1024.0f);
        a.bind(l2);
        a.sw(8, 21, 8); a.sw(9, 21, 12); a.sw(10, 21, 16); a.sw(11, 21, 20); a.sw(12, 21, 24); a.sw(13, 21, 28);
        for (int s = 0; s < P.spills; s++) e.spill(s, 8 + s);
        a.li(22, kind == 3 ? 2 * P.T : P.T); a.li(23, 0);
        int tl = a.here();
        e.struct_at();
        a.call(pm_trace);
        for (int k = 0; k < 8; k++) { e.struct_ld(k, 5 + (k & 3)); if (k & 1) a.op(GVM_I_XOR, 23, 23, 5 + (k & 3)); else a.add(23, 23, 5 + (k & 3)); }
        a.addbnz(22, -1, tl);
        for (int s = 0; s < P.spills; s++) e.fill(s, 8 + s);
        a.op(GVM_I_ADD, 23, 23, 8);
        a.sw(23, 21, 48);
        a.lw(16, 21, 36); a.addi(16, 16, 1); a.sw(16, 21, 36);
        a.opi(GVM_I_AND, 17, 16, 15);
        g_bnei(a, 17, 0, l3);                                   // every 16th: damage
        a.lw(18, 21, 32); a.addi(18, 18, -3); a.sw(18, 21, 32);
        a.bind(l3);
        e.epilogue(3, {1, 22, 23, 24});
        a.ret();
    }
    // ---- pm_trace(r8 x, r9 the caller's struct): a trace, its result as 8 words into the struct
    a.bind(pm_trace);
    {
        e.prologue(4, {1, 22, 23, 24});
        a.mv(22, 9); a.mv(23, 8);
        a.call(trace);
        a.mv(9, 22);
        e.struct_st(0, 5); e.struct_st(1, 23); e.struct_st(2, 16); e.struct_st(3, 17);
        a.op(GVM_I_XOR, 24, 5, 23);
        e.struct_st(4, 24); e.struct_st(5, 18); e.struct_st(6, 19); e.struct_st(7, 0);
        e.epilogue(4, {1, 22, 23, 24});
        a.ret();
    }
    // ---- think 1: AI, the nearest active neighbour of P.ai (loads of other entities, compares, a target)
    a.bind(think[1]);
    {
        e.prologue(3, {1, 22, 23, 24});
        int nl = a.label(), ns = a.label(), nf = a.label(), l4 = a.label();
        a.lw(15, 21, 8);                                    // own x
        for (int s = 0; s < P.spills; s++) e.spill(s, 15);
        a.li(22, P.ai); a.lpca(14, G_CONST); a.lw(23, 14, 12); a.li(24, -1);   // count, best (a large float), target
        a.bind(nl);
        a.opi(GVM_I_MUL, 9, 22, 37); a.add(9, 9, 20); a.op(GVM_I_AND, 9, 9, 49);
        a.opi(GVM_I_SLL, 10, 9, 7); a.add(10, 44, 10);
        a.lw(11, 10, 4); a.lw(12, 10, 8); a.lw(13, 10, 0);
        g_br(a, GVM_CC_BBS, 11, 0, ns);                        // inactive: not a target
        a.fop(GVM_F_SUB, 12, 12, 15); a.fop(GVM_F_SGNJ, 12, 12, 0);   // |dx|
        a.fop(GVM_F_LT, 16, 12, 23);
        g_beq(a, 16, 0, nf);
        a.mv(23, 12); a.mv(24, 9);
        a.bind(nf);
        a.op(GVM_I_ADD, 17, 17, 13);
        a.bind(ns);
        a.addbnz(22, -1, nl);
        for (int s = 0; s < P.spills; s++) e.fill(s, 15);
        a.sw(24, 21, 40);
        a.lw(18, 21, 32); a.opi(GVM_I_SLT, 19, 18, 10);
        g_beq(a, 19, 0, l4);                                   // weak: flee (a flag)
        a.lw(19, 21, 4); a.opi(GVM_I_OR, 19, 19, 4); a.sw(19, 21, 4);
        a.bind(l4);
        a.lw(16, 21, 36); a.addi(16, 16, 1); a.sw(16, 21, 36);
        e.epilogue(3, {1, 22, 23, 24});
        a.ret();
    }
    // ---- think 2: an item (a leaf: a counter, now and then a respawn)
    a.bind(think[2]);
    {
        int l5 = a.label();
        a.lw(16, 21, 36); a.addi(16, 16, 1); a.sw(16, 21, 36);
        a.opi(GVM_I_AND, 17, 16, 63);
        g_bnei(a, 17, 0, l5);
        a.lw(18, 21, 32); a.addi(18, 18, 25); a.sw(18, 21, 32);
        a.bind(l5);
        a.ret();
    }
    // ---- trace(r8 x): down the tree P.D levels; r5 a sum of what it met
    a.bind(trace);
    {
        int tl = a.label(), tf = a.label();
        a.mv(16, 48); a.li(17, P.D); a.li(5, 0);
        a.bind(tl);
        a.lw(18, 16, 0); a.lw(19, 16, 4);
        a.fop(GVM_F_MSB, 18, 8, 18, 19);                    // x * normal - dist
        a.fop(GVM_F_LT, 18, 18, 0);
        g_beq(a, 18, 0, tf);
        a.op(GVM_I_XOR, 5, 5, 17);
        a.bind(tf);
        a.shadd(19, 16, 18, 2); a.lw(19, 19, 8);
        a.add(5, 5, 19);
        a.shadd(16, 48, 19, 4);
        a.addbnz(17, -1, tl);
        a.ret();
    }
    // ---- link(r20 e, r21 entity, r27 frame): a heap node into the hash table
    a.bind(link);
    {
        a.lw(5, 21, 8); a.lw(6, 21, 12);
        a.op(GVM_I_XOR, 5, 5, 6); a.li(7, (int32_t)0x9e3779b1u); a.op(GVM_I_MUL, 5, 5, 7); a.opi(GVM_I_SRL, 5, 5, 22);
        a.shadd(6, 46, 5, 2); a.lw(7, 6, 0);
        a.lpca(9, G_CONST + 28); a.lw(9, 9, 0); a.add(7, 7, 9);   // (a global: 0)
        a.opi(GVM_I_SLL, 8, 20, 2); a.opi(GVM_I_AND, 9, 27, 3); a.add(8, 8, 9); a.opi(GVM_I_SLL, 8, 8, 4); a.add(8, 8, 47);
        a.sw(20, 8, 0); a.sw(7, 8, 4); a.sw(27, 8, 8); a.sw(5, 8, 12);
        a.sw(8, 6, 0);
        a.ret();
    }
    // ---- memcpy(r5 dst, r6 src, r7 bytes): words, four a trip (musl's for aligned)
    a.bind(memcpy_);
    {
        int ml = a.here();
        a.lw(8, 6, 0); a.lw(9, 6, 4); a.lw(10, 6, 8); a.lw(11, 6, 12);
        a.sw(8, 5, 0); a.sw(9, 5, 4); a.sw(10, 5, 8); a.sw(11, 5, 12);
        a.addi(6, 6, 16); a.addi(5, 5, 16);
        a.addbnz(7, -16, ml);
        a.ret();
    }
    return {frame};
}

// verts(r5 output, r6 count): each vertex by its bone's 3 x 4 matrix, a light, clipped by z
static int g_emit_verts(GvmAsm& a) {
    int fn = a.label();
    a.bind(fn);
    int vl = a.label(), nc = a.label();
    a.li(8, (int32_t)G_VERTS); a.mv(9, 5); a.li(10, (int32_t)G_BONES); a.mv(11, 6); a.li(24, 0);
    a.bind(vl);
    a.lw(12, 8, 0); a.lw(13, 8, 4); a.lw(14, 8, 8); a.lw(15, 8, 12);
    a.opi(GVM_I_SLL, 16, 15, 6); a.add(16, 10, 16);
    a.lw(17, 16, 0); a.lw(18, 16, 4); a.lw(19, 16, 8); a.lw(20, 16, 12);
    a.fop(GVM_F_MUL, 21, 17, 12); a.fop(GVM_F_MAD, 21, 18, 13, 21); a.fop(GVM_F_MAD, 21, 19, 14, 21); a.fop(GVM_F_ADD, 21, 21, 20);
    a.lw(17, 16, 16); a.lw(18, 16, 20); a.lw(19, 16, 24); a.lw(20, 16, 28);
    a.fop(GVM_F_MUL, 22, 17, 12); a.fop(GVM_F_MAD, 22, 18, 13, 22); a.fop(GVM_F_MAD, 22, 19, 14, 22); a.fop(GVM_F_ADD, 22, 22, 20);
    a.lw(17, 16, 32); a.lw(18, 16, 36); a.lw(19, 16, 40); a.lw(20, 16, 44);
    a.fop(GVM_F_MUL, 23, 17, 12); a.fop(GVM_F_MAD, 23, 18, 13, 23); a.fop(GVM_F_MAD, 23, 19, 14, 23); a.fop(GVM_F_ADD, 23, 23, 20);
    a.lw(17, 16, 48);                                        // the light: n . l, at least 0, as a byte
    a.fop(GVM_F_MUL, 18, 21, 17); a.fop(GVM_F_MAD, 18, 22, 17, 18); a.fopi(GVM_F_MAX, 18, 18, 0.0f);
    a.fopi(GVM_F_MIN, 18, 18, 255.0f); a.fop(GVM_F_FTOI, 18, 18, 0);
    a.fopi(GVM_F_LT, 19, 23, 1.0f);
    g_beq(a, 19, 0, nc);                                        // behind the near plane: clipped
    a.opi(GVM_I_OR, 18, 18, 0x100); a.addi(24, 24, 1);
    a.bind(nc);
    a.sw(21, 9, 0); a.sw(22, 9, 4); a.sw(23, 9, 8); a.sw(18, 9, 12);
    a.addi(8, 8, 16); a.addi(9, 9, 16); a.addbnz(11, -1, vl);
    a.mv(5, 24);
    a.ret();
    return fn;
}

// ray(r5 first chunk, r6 frame, r7 chunks apart): chunks of 32 pixels of the picture; r60 + 1 a chunk
static uint32_t g_frames() { return g_loop_n > 100000 ? 0x3fffffff : 2; }
static int g_emit_ray(GvmAsm& a, const GRay& R) {
    GEmit e(a, R.stack);
    int fn = a.label();
    a.bind(fn);
    const uint32_t chunks = std::min((uint32_t)(R.w * R.h / 32), g_frames() == 2 ? 256u : ~0u);   // (a check: 256 chunks, WARP's time)
    int cl = a.label(), pl = a.label(), sl = a.label(), skip = a.label(), sky = a.label(), shaded = a.label(), done = a.label();
    int lw2 = 0; while ((1 << lw2) < R.w) lw2++;
    e.prologue(1, {1, 20, 21, 22});
    a.mv(20, 5); a.mv(21, 7); a.li(22, 0);
    a.lpca(14, G_CONST);
    a.bind(cl);
    a.li(8, (int32_t)chunks);
    g_br(a, GVM_CC_BGEU, 20, 8, done);
    a.opi(GVM_I_SLL, 25, 20, 5); a.li(26, 32);
    a.bind(pl);
    a.opi(GVM_I_AND, 9, 25, R.w - 1); a.opi(GVM_I_SRL, 10, 25, lw2);
    a.fop(GVM_F_ITOF, 9, 9, 0); a.fop(GVM_F_ITOF, 10, 10, 0);
    a.li(11, (int32_t)fbits(4.0f / R.w)); a.li(12, (int32_t)fbits(2.0f)); a.li(19, (int32_t)fbits(2.0f * R.h / R.w));   // the pixel's step, the corner
    a.fop(GVM_F_MSB, 9, 9, 11, 12); a.fop(GVM_F_MSB, 10, 10, 11, 19);   // dx, dy (dz 1)
    a.fop(GVM_F_MUL, 13, 9, 9); a.fop(GVM_F_MAD, 13, 10, 10, 13); a.fopi(GVM_F_ADD, 13, 13, 1.0f);   // a = d . d
    a.lw(23, 14, 12); a.li(24, 0);                    // the nearest t, the colour
    a.li(15, (int32_t)G_SPHERES); a.li(16, R.s);
    a.bind(sl);
    a.lw(17, 15, 0); a.lw(18, 15, 4); a.lw(19, 15, 8); a.lw(27, 15, 12);
    a.fop(GVM_F_MUL, 17, 17, 9); a.fop(GVM_F_MAD, 17, 18, 10, 17); a.fop(GVM_F_ADD, 17, 17, 19);   // b = d . c
    a.fop(GVM_F_MUL, 18, 13, 27); a.fop(GVM_F_MSB, 18, 17, 17, 18);   // disc = b * b - a * cc
    a.fop(GVM_F_LT, 19, 18, 0);
    g_bnei(a, 19, 0, skip);                                    // a miss
    a.fop(GVM_F_SQRT, 61, 18, 0);                           // (the root, a sink: the card's own is not exact)
    a.lw(19, 14, 24); a.fop(GVM_F_NMSB, 18, 18, 19, 17);    // t ~ b - disc * k
    e.spill(2, 18); e.spill(3, 17);                          // the hit's record on the stack (nxray's stores)
    a.fop(GVM_F_LT, 19, 18, 23);
    g_beq(a, 19, 0, skip);
    a.mv(23, 18); a.lw(24, 15, 16);
    a.bind(skip);
    a.addi(15, 15, 32); a.addbnz(16, -1, sl);
    for (int s = 0; s < R.spill; s++) e.spill(s, 23 - s);
    a.lw(17, 14, 12);
    g_beq(a, 24, 0, sky);
    a.fop(GVM_F_LT, 18, 23, 17);
    g_beq(a, 18, 0, sky);
    a.fop(GVM_F_RSQRT, 62, 13, 0);                          // (a sink)
    a.fopi(GVM_F_MUL, 18, 23, 16.0f); a.fopi(GVM_F_MIN, 18, 18, 255.0f); a.fopi(GVM_F_MAX, 18, 18, 0.0f);
    a.fop(GVM_F_FTOI, 18, 18, 0);
    a.op(GVM_I_MUL, 18, 18, 24); a.opi(GVM_I_SRL, 18, 18, 8);
    a.j(shaded);
    a.bind(sky);
    a.op(GVM_I_XOR, 18, 25, 6); a.opi(GVM_I_AND, 18, 18, 0xff);
    a.bind(shaded);
    for (int s = 0; s < R.spill; s++) e.fill(s, 23 - s);
    a.li(19, (int32_t)G_FB); a.shadd(19, 19, 25, 2); a.sw(18, 19, 0);
    a.add(22, 22, 18);
    a.addi(25, 25, 1); a.addbnz(26, -1, pl);
    a.addi(60, 60, 1);
    a.add(20, 20, 21);
    a.j(cl);
    a.bind(done);
    a.mv(5, 22);
    e.epilogue(1, {1, 20, 21, 22});
    a.ret();
    return fn;
}

// The data every case shares: the tree, spheres, vertices and bones, constants, and logic instances 0..n-1 of E entities
static void g_data(std::vector<std::pair<uint32_t, uint32_t>>& data, int instances, int E, const GRay& R) {
    Rng g{0x5eed1234u};
    for (uint32_t i = 0; i < 4096; i++) {
        float nx = (float)((int)g.below(33) - 16) / 16.0f, d = (float)((int)g.below(801) - 400);
        data.push_back({G_WORLD + 16 * i, fbits(nx)}); data.push_back({G_WORLD + 16 * i + 4, fbits(d)});
        data.push_back({G_WORLD + 16 * i + 8, (2 * i + 1) & 4095}); data.push_back({G_WORLD + 16 * i + 12, (2 * i + 2) & 4095});
    }
    const uint32_t cst[8] = {fbits(1.0f / 32), fbits(0.75f), fbits(20.0f), fbits(1e30f), fbits(1.0f / 64), fbits(2.0f), fbits(0.125f), 0};
    for (int k = 0; k < 8; k++) data.push_back({G_CONST + 4 * k, cst[k]});
    (void)R;
    for (int s = 0; s < 16; s++) {
        float cx = (float)((int)g.below(65) - 32) / 8.0f, cy = (float)((int)g.below(49) - 24) / 16.0f, cz = (float)(3 + g.below(6));
        float r = (float)(16 + g.below(17)) / 8.0f;   // (big: a fifth of the tests hit, most pixels)
        float cc = cx * cx + cy * cy + cz * cz - r * r;
        uint32_t w[8] = {fbits(cx), fbits(cy), fbits(cz), fbits(cc), 0x10101u * (1 + g.below(255)), 0, 0, 0};
        for (int k = 0; k < 8; k++) data.push_back({G_SPHERES + 32 * s + 4 * k, w[k]});
    }
    for (uint32_t v = 0; v < 4096; v++) {
        uint32_t w[4] = {fbits((float)((int)g.below(201) - 100) / 8), fbits((float)((int)g.below(201) - 100) / 8), fbits((float)((int)g.below(201) - 100) / 8), g.below(32)};
        for (int k = 0; k < 4; k++) data.push_back({G_VERTS + 16 * v + 4 * k, w[k]});
    }
    for (uint32_t b = 0; b < 32; b++)
        for (int k = 0; k < 16; k++) data.push_back({G_BONES + 64 * b + 4 * k, fbits((float)((int)g.below(17) - 8) / 8)});
    for (int in = 0; in < instances; in++)
        for (int e = 0; e < E; e++) {
            uint32_t at = G_DATA + in * G_INST + 128 * e, w[32];
            for (int k = 0; k < 32; k++) w[k] = g.next();
            w[0] = g.below(4); w[1] = g.below(8) == 0 ? 1 : 0;
            for (int k = 2; k <= 4; k++) w[k] = fbits((float)((int)g.below(1001) - 500) / 4);
            w[4] = fbits((float)g.below(400) / 4);
            for (int k = 5; k <= 7; k++) w[k] = fbits((float)((int)g.below(201) - 100) / 4);
            w[8] = 50 + g.below(100); w[9] = g.below(64);
            for (int k = 0; k < 32; k++) data.push_back({at + 4 * k, w[k]});
        }
}

// A scene's programs: core 0's (or a lone kernel's), and the workers' (core 1's; cores 2 on jump there)
static void g_scene(std::vector<GvmAsm>& p, std::vector<std::pair<uint32_t, uint32_t>>& data, const GScene& S) {
    const uint32_t ncores = (uint32_t)p.size(), busy = (uint32_t)S.jobs.size();
    const int Ew = S.worker.E;   // (the instances' size: the largest a job asks)
    int maxE = S.core0.E;
    for (auto& j : S.jobs) if (j.kind == 1 && j.size > maxE) maxE = j.size;
    g_data(data, 1 + (int)busy, maxE ? maxE : 1, S.ray);
    (void)Ew;
    // ---- core 0
    {
        GvmAsm& a = p[0];
        a.li(2, (int32_t)(G_STACK + 0x4000 - 64)); a.li(60, 0); a.li(4, 0);
        int fl = a.label(), main_ = a.label();
        a.j(main_);
        GLogicCode lc = g_emit_logic(a, S.core0);
        int rayfn = S.rayW == 0 && ncores == 1 && S.core0.E == 0 ? g_emit_ray(a, S.ray) : -1;
        // the jobs of the frame before: done when each answer's done word is the frame's number (r4)
        auto wait_all = [&]() {
            if (S.rayW) {
                int wl = a.label(), ok = a.label();
                a.li(48, 1);
                a.bind(wl);
                a.li(49, (int32_t)G_ANS); a.shadd(49, 49, 48, 4);
                int again = a.here();
                a.lw(50, 49, 0);
                g_beq(a, 50, 4, ok);
                a.x(GVM_X_SYNC);
                a.j(again);
                a.bind(ok);
                a.addi(48, 48, 1);
                a.li(51, S.rayW + 1);
                g_bne(a, 48, 51, wl);
            }
            for (uint32_t k = 1; k <= busy; k++) {
                int again = a.label(), ok = a.label();
                a.li(49, (int32_t)(G_ANS + 16 * k));
                a.bind(again);
                a.lw(50, 49, 0);
                g_beq(a, 50, 4, ok);
                a.x(GVM_X_SYNC);
                a.j(again);
                a.bind(ok);
            }
        };
        a.bind(main_);
        a.li(3, (int32_t)(S.rayW && g_frames() == 2 ? 1 : g_frames()));   // (a check: one frame of 191 workers is minutes on WARP)
        a.bind(fl);
        wait_all();
        // post this frame's jobs: {seq = frame + 1, kind, a0, a1}, the seq last
        a.addi(51, 4, 1);
        for (uint32_t k = 1; k <= busy; k++) {
            const GJob& j = S.jobs[k - 1];
            a.li(48, (int32_t)(G_MBOX + 16 * k));
            a.li(52, j.kind);
            a.li(53, (int32_t)(j.kind == 1 ? G_DATA + k * G_INST : G_VOUT + k * 0x20000));
            a.li(54, j.size);
            a.sw(52, 48, 4); a.sw(53, 48, 8); a.sw(54, 48, 12); a.sw(51, 48, 0);
        }
        if (S.rayW) {
            int pl = a.label();
            a.li(48, 1); a.li(52, 3);
            a.bind(pl);
            a.li(49, (int32_t)G_MBOX); a.shadd(49, 49, 48, 4);
            a.addi(53, 48, -1);
            a.sw(52, 49, 4); a.sw(53, 49, 8); a.sw(4, 49, 12); a.sw(51, 49, 0);
            a.addi(48, 48, 1);
            a.li(54, S.rayW + 1);
            g_bne(a, 48, 54, pl);
        }
        // core 0's own work: its logic, or a lone kernel's (g_ray1: every chunk)
        if (S.core0.E) { a.li(5, (int32_t)G_DATA); a.li(6, S.core0.E); a.mv(7, 4); a.call(lc.frame); }
        else if (rayfn >= 0) { a.li(5, 0); a.mv(6, 4); a.li(7, 1); a.call(rayfn); }
        a.addi(4, 4, 1);
        g_bne(a, 4, 3, fl);
        // the end: the last jobs, then every worker told to stop (kind 0)
        wait_all();
        if (ncores > 1) {
            int ql = a.label();
            a.li(48, 1); a.li(51, 0x7fffffff);
            a.bind(ql);
            a.li(49, (int32_t)G_MBOX); a.shadd(49, 49, 48, 4);
            a.sw(0, 49, 4); a.sw(51, 49, 0);
            a.addi(48, 48, 1);
            a.li(54, (int32_t)ncores);
            g_bne(a, 48, 54, ql);
        }
        a.halt();
    }
    if (ncores == 1) return;
    // ---- the workers: core 1's program; cores 2 on jump to it
    {
        GvmAsm& a = p[1];
        int wl = a.label(), got = a.label(), k2 = a.label(), k3 = a.label(), done = a.label(), quit = a.label(), main_ = a.label();
        a.j(main_);
        GLogicCode lc = g_emit_logic(a, S.worker);
        int vfn = g_emit_verts(a);
        int rfn = g_emit_ray(a, S.ray);
        a.bind(main_);
        a.mfsr(3, GVM_SR_COREID);
        a.opi(GVM_I_SLL, 5, 3, 14); a.li(2, (int32_t)(G_STACK + 0x4000 - 64)); a.add(2, 2, 5);
        a.li(56, (int32_t)G_MBOX); a.shadd(56, 56, 3, 4); a.li(3, 0);
        a.bind(wl);
        a.lw(7, 56, 0);
        g_bne(a, 7, 3, got);
        // nothing: what the firmware's timer trap costs a pass (worker.S fw_irq and fw_back), then the pass's end
        a.mfsr(58, GVM_SR_TIME_LO); a.addi(58, 58, 1);
        for (int i = 0; i < S.idleOps; i++) {
            if (i % 3 == 0) a.mfsr(59, GVM_SR_COREID);
            else if (i % 3 == 1) a.opi(GVM_I_SLL, 59, 59, 4);
            else a.lw(59, 56, 4);
        }
        a.mtsr(GVM_SR_SCRATCH, 58);
        a.x(GVM_X_SYNC);
        a.j(wl);
        a.bind(got);
        a.lw(57, 56, 4); a.lw(5, 56, 8); a.lw(6, 56, 12);
        g_beq(a, 57, 0, quit);
        a.mv(3, 7);
        g_bnei(a, 57, 1, k2);
        a.call(lc.frame); a.j(done);
        a.bind(k2);
        g_bnei(a, 57, 2, k3);
        a.call(vfn); a.j(done);
        a.bind(k3);
        a.li(7, S.rayW > 0 ? S.rayW : 1); a.call(rfn);
        a.bind(done);
        a.sw(5, 56, 0x1004); a.sw(3, 56, 0x1000);
        a.j(wl);
        a.bind(quit);
        a.sw(7, 56, 0x1000);
        a.halt();
    }
    for (uint32_t c = 2; c < ncores; c++) {
        GvmAsm& a = p[c];
        a.li(5, (int32_t)p[1].base);
        a.jr(5);
    }
}

// ---- The cases, and the RISC-V machine's numbers for them (frames a second, gamebench_rv on rvc_opt, this card)
static double g_rv_of(const char* name) {
    static const struct { const char* n; double v; } L[] = {{"g_none", 0}};
    for (auto& l : L) if (!strcmp(l.n, name)) return l.v;
    return 0;
}
static GScene g_scene_of(const std::string& name) {
    GScene S;
    std::string n = name.substr(0, name.find('_', 2));   // (the form after a second '_': r, f, c)
    std::string form = name.size() > n.size() ? name.substr(n.size() + 1) : "";
    if (n == "g_logic") { S.core0.E = 512; }
    else if (n == "g_verts") { S.core0.E = 0; }
    else if (n == "g_ray1") { S.ray.w = 64; S.ray.h = 32; }
    else if (n == "g_q3") { S.core0.E = 64; S.worker = S.core0; S.jobs = {{1, 256}, {2, 2048}, {1, 64}}; S.idle = 2; }
    else if (n == "g_hl") { S.core0.E = 512; S.worker = S.core0; S.jobs = {{1, 128}, {2, 2048}, {2, 2048}, {1, 32}}; S.idle = 40; }
    else if (n == "g_ray") { S.core0.E = 16; S.worker = S.core0; S.rayW = 191; }
    int stack = form == "r" ? G_REGS : form == "f" ? G_FILE : G_MEM;
    S.core0.stack = S.worker.stack = S.ray.stack = stack;
    S.core0.copyop = S.worker.copyop = form == "c";
    return S;
}
static uint32_t g_cores_of(const GScene& S) { return 1 + (uint32_t)S.jobs.size() + (uint32_t)S.idle + (uint32_t)S.rayW; }
static void game_cases(std::vector<Case>& v) {
    for (const char* name : {"g_logic", "g_verts", "g_ray1", "g_q3", "g_hl", "g_ray",
                             "g_logic_r", "g_logic_f", "g_logic_c", "g_q3_f", "g_hl_r", "g_hl_f", "g_hl_c", "g_ray_f"}) {
        std::string nm = name;
        GScene S = g_scene_of(nm);
        bool lone = nm.rfind("g_logic", 0) == 0 || nm == "g_verts" || nm == "g_ray1";
        Case c{name, lone ? 1u : g_cores_of(S), true, [nm](std::vector<GvmAsm>& p, auto& data, uint32_t) {
            GScene S = g_scene_of(nm);
            if (nm == "g_verts") {   // one core: verts of 4,096, a unit each call
                GvmAsm& a = p[0];
                g_data(data, 0, 1, S.ray);
                int main_ = a.label();
                a.li(2, (int32_t)(G_STACK + 0x4000 - 64)); a.li(60, 0);
                a.j(main_);
                int vfn = g_emit_verts(a);
                a.bind(main_);
                a.li(3, (int32_t)g_frames()); a.li(4, 0);
                int fl = a.here();
                a.li(5, (int32_t)G_VOUT); a.li(6, 4096); a.call(vfn);
                a.addi(60, 60, 1); a.addi(4, 4, 1);
                g_bne(a, 4, 3, fl);
                a.halt();
                return;
            }
            g_scene(p, data, S);
        }};
        c.named = true; c.set = nm.find('_', 2) == std::string::npos ? "game" : "game_x"; c.big = true;
        c.skip_regs = G_SKIP_REGS; c.skip_ops = !lone; c.units_reg = 60;
        c.check_ticks = lone ? 997 : S.rayW ? 4093 : 1999;
        c.unit_div = nm == "g_verts" ? 1 : S.core0.E ? S.core0.E : S.ray.w * S.ray.h / 32;
        if (S.core0.stack == G_FILE) c.xr_need = 128;
        if (nm.rfind("g_hl", 0) == 0) c.passes = 1500;
        v.push_back(c);
    }
    // the one-core kernels as gpu_vm's clang compiles them (gamebench_rv/gvm_lone.c, build_gvm.sh; the set game_cc): ns a
    // bundle, as cases.h's real programs; work a second is 1 / (ns a bundle x bundles a frame, gvm_cpu --stats: GAMEBENCH.md)
    for (const char* name : {"g_logic_cc", "g_verts_cc", "g_ray1_cc"}) {
        static const char* pk[3] = {"gb_kind=1,1;gb_frames=1,1000000", "gb_kind=2,2;gb_frames=1,1000000", "gb_kind=3,3;gb_frames=1,1000000"};
        int k = name[2] == 'l' ? 0 : name[2] == 'v' ? 1 : 2;
        Case c{name, 1, true, [](std::vector<GvmAsm>&, std::vector<std::pair<uint32_t, uint32_t>>&, uint32_t) {}};
        c.elf = "build/gamebench/gvm_lone.elf"; c.pokes = pk[k]; c.named = true; c.ns = true; c.set = "game_cc";
        c.passes = 2000;   // (the data made first, ~640 thousand bundles: under 5% of the timing)
        v.push_back(c);
    }
}
