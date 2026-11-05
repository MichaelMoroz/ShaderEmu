// The tick: RV32IMAF for Linux without an MMU, every core of the machine, in one fragment shader
// (README.md). Written as one loop that
// tests the kinds of instruction most common first, arithmetic as one test with the result
// chosen by selects, one indexed register write; a general step of its own for the rest. The
// state texture is the machine's (docs/machine/multicore.md): RAM from row 64, and in rows 0-63 a
// slot of 64 x 8 texels for each core; what a core stores goes into its write cache
// (three tables of buckets of three texels), and the commit pass puts it into RAM.
//
// A pixel of the tick is eight texels of a core's state, one in each of eight targets; every
// pixel of a core runs the whole pass and writes its own eight. (TICK_UNPACK: this file without
// its pass, for unpack.hlsl.)
//
// FAST (off; rvc_harness --define FAST, rvc_cpu --fast): a load or store in RAM not on its size takes its word's bytes
// (no trap, no second word), fdiv is the card's a / b, an sc always succeeds, and an opcode of no group is no
// instruction. Programs it breaks: docs/machine/tick-costs.md, "FAST".

Texture2D<uint4> _SelfTexture2D;
Texture2D<float4> _Data_ROM;   // the ROM at 0x40000000
uniform uint _Init, _Ticks, _PlayerID;
// A material's numbers are floats, so each clock word comes as two 16-bit halves.
uniform uint _Rtc0Lo, _Rtc0Hi, _Rtc1Lo, _Rtc1Hi;
#define _RTC0 ((_Rtc0Lo & 0xffff) | (_Rtc0Hi << 16))
#define _RTC1 ((_Rtc1Lo & 0xffff) | (_Rtc1Hi << 16))
uniform uint _MtimeLoLo, _MtimeLoHi;   // the timer: the host works it out, a pixel with no doubles (rvc_time.h)
uniform float4 _Time;

#ifndef CORES
#define CORES 256
#endif
#define TEX(p) _SelfTexture2D[p]
#define RAM_ADDR(t) uint2((t) & 2047, 64 + ((t) >> 11))
#ifndef STATE_TEX_HEIGHT
#define STATE_TEX_HEIGHT 9280u   // the state texture's rows: 64 of state, then RAM to 0x92000000
#endif
#define RAM_MAX (2048u * (STATE_TEX_HEIGHT - 64u) * 16u)
// A core's state, as texels numbered 64 to a row of core 0's block: 0-2 the CSRs, 8 the store a
// full cache had no room for and the privilege, 10 the timer's compare, 28 the stall, the count
// and the passes, 29-36 x1-x31 and pc, 37 the reservation and a copy's source and destination,
// 38 its length, 41 the stores' addresses or-ed, their 4 MB bands and the worker's word, from 44
// the write cache (a texel of tags, then three texels, a bucket), then the float registers.
#define L1_AT 44
#define FP_AT 492                                    // after core 0's 112 buckets: the numbering's (lin_of)
#define CORE0_BITS 6                                 // core 0's cache: tables of 2^6 (its floats then right after)
#define BUCKETS(bits) ((7u << (bits)) >> 2)          // tables of 2^bits, 2^(bits-1) and 2^(bits-2)
#define H1(t, n) ((((t) >> 3) ^ ((t) << ((n) - 3)) ^ ((t) >> (n))) & ((1u << (n)) - 1))
#define H2(t, n) ((((t) >> 2) ^ ((t) << ((n) - 2)) ^ ((t) >> ((n) + 3))) & ((1u << (n)) - 1))
#define BUCKET2(t, n) ((n) + ((n) >> 1) + (H2(t, bits - 1) & (((n) >> 2) - 1)))   // the third table's bucket of t
#define LOAD_UP(f3) (((f3) & 2) ? 0u : ((f3) & 1) ? 16u : 24u)   // the shift up that leaves a load's sign at the top (by selects)
#define STALL_MEM 3
#define STALL_FENCE 7
#define STALL_COPY 8
#define STALL_WFI 9
#define STALL_FILL 10
// Lanes (docs/machine/lanes.md): a worker that wrote CSR 0x0b8 stalls with STALL_LANES_START, and
// its passes are lanes' until its job is through (STALL_LANES); an index's end in a lane, and its failure
#define STALL_LANES 12
#define STALL_LANES_START 13
#define STALL_LANE_RET 14
#define STALL_LANE_FAIL 15
#define MBOX 0x00C00000u   // the workers' mailboxes, as a RAM offset
// The field (docs/machine/lanes.md, "The field"): up to FIELD_MAX lanes of no core's, a pixel each in
// rows 8 and 9 of the targets (under the cores' row), their eight texels in tiles 5 and 6 of the slots
// from the ninth on, which no worker's reach (a worker has 2^5 or less: five tiles), sixteen lanes a
// slot. A program posts a job in the mailboxes' page (FIELD_JOB_T: the
// descriptor's address, the job's number, how many lanes it wants). The first pixel of every 64 is a
// group's leader, which runs no index and puts how its 63 lanes stand in FIELD_STATUS_T + its group.
// The host draws it (_LaneField) while a job is posted, and the commit has a point a bucket of it.
uniform uint _LaneField;
#define FIELD_MAX 3968
#define FIELD_JOB_T 0x0C0190u
#define FIELD_STATUS_T 0x0C01A0u
#define MC_START 0x5553434du
#define MC_STOP 0x5453434du

// ---- The geometry (docs/machine/multicore.md, "The geometry"): every core has a slot of 64 x 8 texels in
// the state's rows, core k's at ((k & 31) * 64, (k >> 5) * 8), and a block of 8 x 8 pixels in the targets,
// at (k * 8, 0): one row of cores, all found by shifts, drawn by one triangle over the targets (a quad a core split each block
// along its diagonal and mixed cores in a warp), what no core runs discarded at the top of frag: a warp is 4 x 8 pixels of one core. Core 0's texels are in rows; a worker's in whole tiles
// of 8 x 8, a tile a column of its block (rows of 64 cost its loads: Quake 50%), a pixel eight texels of a tile's row.
#define GEO_TEXEL 0x10003du
#define TICK_W 2048
#define TICK_H 16
static uint4 g;
static uint g_count;
static uint core, bits, rows_now;   // the core whose state is read

uint rows_of(uint b) { return (L1_AT + 8 + BUCKETS(b) * 4 + 63) >> 6; }
void geo_read() {
    g = TEX(RAM_ADDR(GEO_TEXEL));
    g_count = (g.r & 1) == 0 && g.b >= 3 && g.b <= 5 ? min(g.g, (uint)CORES - 1) : 0;
}
void select_core(uint k) {
    core = k;
    bits = k == 0 ? CORE0_BITS : k <= g_count ? g.b : 0;
    rows_now = rows_of(bits);
}
// Texel lin (core 0's numbering) of the selected core: a worker keeps its floats after its cache.
uint4 st(uint lin) {
    uint end = L1_AT + BUCKETS(bits) * 4, w = lin >= FP_AT ? end + lin - FP_AT : lin;
    if (lin >= end && lin < FP_AT) return 0;
    if (core == 0) return TEX(uint2(w & 63, w >> 6));
    return TEX(uint2(((core & 31) << 6) + ((w >> 6) << 3) + (w & 7), ((core >> 5) << 3) + ((w >> 3) & 7)));
}
// which texel (core 0's numbering) texel w of the selected core's rows is; 4095 for none
uint lin_of(uint w) {
    uint end = L1_AT + BUCKETS(bits) * 4;
    return w < end ? w : w < end + 8 ? FP_AT + w - end : 4095;
}

uint idx(uint4 v, uint i) { return (i & 2) ? ((i & 1) ? v.w : v.z) : ((i & 1) ? v.y : v.x); }
uint4 put(uint4 v, uint i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }

// ---- The processor's state in this pass
static uint xr[65];   // x0-x31, f0-f31 as bits, and [64] for writes that must not land
static uint pc, priv, stall, clock, mstatus, mie, mip, stvec, sscratch, sepc, scause, stval, scounteren, fcsr;
static uint tcmp_lo, tcmp_hi, now_lo, now_hi, res_en, res_addr, msrc, mdst, mn, ovf_addr, ovf_val, bloom, dirty, mc_word;
static uint regs = 0;   // the 16 KB of RAM the write cache has texels of (bit (o >> 14) & 31, regions 512 KB apart sharing one): a load
                        // elsewhere searches nothing (the bloom, the stored addresses or-ed, let most loads through)
static bool quiet;            // nothing the general step looks at after an instruction has changed
static bool trapped;
static uint tcause, tval;
static uint lane_word = 0;    // texel 28.w: a lanes job's descriptor while it runs, then how it ended (1 whole, 2 a lane stuck, 4 an index not through)
static bool lane_mode = false;   // this pixel runs a lane's indices
static bool lane_pixel = false;  // this pixel's texels are a lane's two buckets in this pass
// a lane's job: its number, of how many, its first index's place in its sequence and how many it made
// in this pass, and the texels they wrote; the function, the stub it returns to, the stack's top, the
// context, the first index and the count, the output's first RAM texel and texels an index, the stuck lanes' texels
// (lane_j: the lane's place in its order; bit 29 once an index of the job returned not through, bit 30 once it
// skipped one, kept there and not in statics of their own, which every core's loop would carry; bit 31 when it stopped)
static uint lane_L, lanes_n, lane_j, lane_k, lane_used, lane_fn, lane_stub, lane_sp, lane_ctx, lane_first, lane_count;
static uint lane_out, lane_T, lane_status, lane_ran;
static uint lane_job = 0;   // the field's job's number, in a stuck lane's word (a worker's lanes: 0)
static bool lane_stuck = false;
static uint budget;           // the fast loop's instructions at most (a store with no room makes it 0)

// ---- Memory. The cache is a local array of the pixel, its tags at 4b, its texels after;
// a bit a bucket says it was written in this pass (it starts undefined otherwise). The bits are a
// vector in registers: as a local array of four they cost a load what the card's local memory costs.
#define C_N 448
// (fxc2: not zeroed at every pass; the cache's entries are read only where its bits say so. xr, above, is zeroed
// though all of it is written before it is read: without that the three loads were 0.6 to 0.8% slower.)
#pragma static_noinit
static uint4 c[C_N];
static uint4 ot[8];
static uint4 occv = 0;
#define OCC(b) (((idx(occv, (b) >> 5) >> ((b) & 31)) & 1) != 0)
#define OCC_SET(b) occv = put(occv, (b) >> 5, idx(occv, (b) >> 5) | (1u << ((b) & 31)))
static uint dr_t = 0xffffffff;   // the RAM texel read last for data, as the texture has it
static uint4 dr_v;
// Most searches are for a texel just used (the next word of a save, a structure, a copy): the two
// found or made last are remembered with where they are, and the texel looked for last and not found.
// (A bit a texel by a hash, saying the cache was given it, spared searches and cost more than they
// did: its 32 words were a local array, read on every load the bloom let through.)
static uint m1_t = 0xffffffff, m1_at = 0, m2_t = 0xffffffff, m2_at = 0, absent_t = 0xffffffff;   // (absent_t: looked for last and not there)
void remember(uint t, uint at) { m2_t = m1_t; m2_at = m1_at; m1_t = t; m1_at = at; }
uint slot(uint4 tags, uint tag) { return tags.x == tag ? 1 : tags.y == tag ? 2 : tags.z == tag ? 3 : 0; }
// where the cache holds RAM texel t (4b + entry), or 0: a table is looked in when the one before is full
uint find(uint t) {
    // (the tables one after another, each one's bucket worked out only when it is looked in)
    uint n = 1u << bits, b = t & (n - 1), found = 0;
    bool more = false;
    [branch] if (OCC(b)) {
        uint4 tags = c[4 * b];
        uint e = slot(tags, t + 1);
        found = e != 0 ? 4 * b + e : 0;
        more = e == 0 && tags.z != 0;
    }
    [branch] if (more) {
        b = n + H1(t, bits - 1);
        more = false;
        [branch] if (OCC(b)) {
            uint4 tags = c[4 * b];
            uint e = slot(tags, t + 1);
            found = e != 0 ? 4 * b + e : 0;
            more = e == 0 && tags.z != 0;
        }
        [branch] if (more) {
            b = BUCKET2(t, n);
            [branch] if (OCC(b)) {
                uint e = slot(c[4 * b], t + 1);
                found = e != 0 ? 4 * b + e : 0;
            }
        }
    }
    return found;
}
// a word of RAM (o its offset, on a word); the texture is asked first, the cache meanwhile
uint ram_word(uint o) {
    uint t = o >> 4;
    [branch] if (t != dr_t) { dr_v = TEX(RAM_ADDR(t)); dr_t = t; }
    bool maybe = ((regs >> ((o >> 14) & 31)) & 1) != 0;
    uint at = t == m1_t ? m1_at : t == m2_t ? m2_at : 0;   // (the remembered texels are the cache's, whatever the regions say)
    [branch] if (maybe && at == 0 && t != absent_t) {
        at = find(t);
        if (at != 0) remember(t, at); else absent_t = t;
    }
    uint4 tx = at != 0 ? c[at] : dr_v;
    uint w = idx(tx, (o >> 2) & 3);
    return w;   // (a store with no room ends the pass: no load reads after it)
}
// `bits` into the word at RAM offset o under `mask`: the tables looked in one after another, each
// one's bucket worked out only when the one before is full; a whole word into a texel the cache
// has is written without reading what was there
void ram_store_nc(uint o, uint v, uint mask) {   // ram_store for an o known to be in RAM
    uint t = o >> 4, wi = (o >> 2) & 3, tag = t + 1, n = 1u << bits, b0 = 0, b1 = 0, f0 = 0, f1 = 0;
    uint4 tags0 = 0, tags1 = 0;
    uint at = t == m1_t ? m1_at : t == m2_t ? m2_at : 0;
    [branch] if (at == 0) {
        [branch] if (t != dr_t) { dr_v = TEX(RAM_ADDR(t)); dr_t = t; }   // (asked before the search: a new texel's store needs it)
        b0 = t & (n - 1);
        [branch] if (OCC(b0)) tags0 = c[4 * b0];
        uint e0 = slot(tags0, tag), e1 = 0;
        f0 = slot(tags0, 0);
        [branch] if (e0 == 0 && f0 == 0) {
            b1 = n + H1(t, bits - 1);
            [branch] if (OCC(b1)) tags1 = c[4 * b1];
            e1 = slot(tags1, tag); f1 = slot(tags1, 0);
            [branch] if (e1 == 0 && f1 == 0) {
                b1 = BUCKET2(t, n);
                tags1 = 0;
                [branch] if (OCC(b1)) tags1 = c[4 * b1];
                e1 = slot(tags1, tag); f1 = slot(tags1, 0);
            }
        }
        at = e0 != 0 ? 4 * b0 + e0 : e1 != 0 ? 4 * b1 + e1 : 0;
        if (at != 0) remember(t, at);
    }
    [branch] if (at != 0 && mask == 0xffffffff) {
        bloom |= o;
        [branch] if (wi < 2) { if (wi == 0) c[at].x = v; else c[at].y = v; }
        else { if (wi == 2) c[at].z = v; else c[at].w = v; }
    } else {
        // (no return in it: a return in a function inlined in the loop is a flag tested after it)
        uint4 texel = at != 0 ? c[at] : dr_v;   // (the RAM texel was asked with the search)
        uint cur = at == 0 && ovf_addr == o && (o & bloom) == o ? ovf_val : idx(texel, wi);
        uint val = (cur & ~mask) | (v & mask);
        [branch] if (val != cur) {
            bloom |= o;
            dirty |= 1u << ((o >> 22) & 31);
            texel = put(texel, wi, val);
            [branch] if (at != 0) {
                c[at] = texel;
            } else if (f0 != 0 || f1 != 0) {
                uint b = f0 != 0 ? b0 : b1, f = f0 != 0 ? f0 : f1;
                c[4 * b] = put(f0 != 0 ? tags0 : tags1, f - 1, tag);
                c[4 * b + f] = texel;
                OCC_SET(b);
                regs |= 1u << ((o >> 14) & 31);
                remember(t, 4 * b + f);
                absent_t = 0xffffffff;
            } else {
                ovf_addr = o; ovf_val = val;   // made by the commit, which ends the pass here
                stall = STALL_MEM; budget = 0;
            }
        }
    }
}
void ram_store(uint o, uint v, uint mask) { [branch] if (o < RAM_MAX) ram_store_nc(o, v, mask); }   // (any o: past RAM no store)
// a word anywhere (a on a word): RAM, the ROM, the clock chip, the timer
uint load_word(uint a) {
    [branch] if (a & 0x80000000) {
        uint o = a & 0x7fffffff;
        return o < RAM_MAX ? ram_word(o) : 0;
    }
    [branch] if (a & 0x40000000) {
        uint t = (a - 0x40000000) >> 4;
        return asuint(_Data_ROM[uint2(t & 2047, t >> 11)])[(a >> 2) & 3];   // (the ROM: a texel a pixel, from the first row)
    }
    return a == 0x030007f8 ? _RTC0 : a == 0x030007fc ? _RTC1 : a == 0x02004000 ? tcmp_lo : a == 0x02004004 ? tcmp_hi :
           a == 0x0200bff8 ? now_lo : a == 0x0200bffc ? now_hi : 0;
}
// a store of `size` bytes anywhere: RAM (a byte at a time when it is not one whole word), the timer
void store_any(uint a, uint v, uint size) {
    [branch] if ((a & 0x80000000) == 0) {
        quiet = false;
        if (size == 4 && a == 0x02004000) tcmp_lo = v;
        if (size == 4 && a == 0x02004004) tcmp_hi = v;
        return;
    }
    bool whole = size == 4 && (a & 3) == 0;
    [loop] for (uint i = 0; i < (whole ? 1 : size); i++) {
        uint o = (a + i) & 0x7fffffff, sh = (o & 3) * 8;
        ram_store(o & ~3u, whole ? v : ((v >> (8 * i)) & 0xff) << sh, whole ? 0xffffffff : 0xffu << sh);
    }
}

// ---- Instructions. The fetch reads the texture (never the cache: code a pass stores runs from
// the next, after fence.i, which ends the pass); a texel is four instructions, the next is read too.
static uint fw_t = 0xffffffff;
static uint pre_w;           // the word the fast loop fetched and left to the general step
static bool pre_valid = false;
static uint4 fw0, fw1;
uint fetch(uint a) {
    uint t = (a & 0x7fffffff) >> 4;
    // (no test of "this texel is the window's": a jump within the texel reads it again, 3 in 1,000,
    // and every other entry passes a test less)
    [branch] if (t == fw_t + 1) fw0 = fw1; else fw0 = TEX(RAM_ADDR(t));   // (an if: a ?: would read the texel either way)
    fw1 = TEX(RAM_ADDR(t + 1)); fw_t = t;
    return idx(fw0, (a >> 2) & 3);
}
uint imm_i(uint w) { return asuint(asint(w) >> 20); }
uint imm_s(uint w) { return (asuint(asint(w) >> 20) & 0xffffffe0) | ((w >> 7) & 31); }
uint imm_b(uint w) { return asuint(asint(w & 0x80000000) >> 19) | ((w & 0x80) << 4) | ((w >> 20) & 0x7e0) | ((w >> 7) & 0x1e); }
uint imm_j(uint w) { return asuint(asint(w & 0x80000000) >> 11) | (w & 0xff000) | ((w >> 9) & 0x800) | ((w >> 20) & 0x7fe); }
uint mulhu(uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16, lh = al * bh, hl = ah * bl;
    return ah * bh + (lh >> 16) + (hl >> 16) + ((((al * bl) >> 16) + (lh & 0xffff) + (hl & 0xffff)) >> 16);
}
// M by its kind, a product first: a division is dear, and one result made of eight costs every mul all of them
uint op_m(uint f3, uint a, uint b) {
    [branch] if (f3 == 0) return a * b;
    [branch] if (f3 < 4) {
        uint hu = mulhu(a, b);
        return f3 == 1 ? hu - ((a >> 31) ? b : 0) - ((b >> 31) ? a : 0) : f3 == 2 ? hu - ((a >> 31) ? b : 0) : hu;
    }
    // (D3D's division by zero is all ones, and the signed ones are an unsigned one of the magnitudes)
    bool zero = b == 0;
    int sa = asint(a), sb = asint(b);
    [branch] if (f3 == 4) return zero ? 0xffffffff : asuint(sa / sb);
    [branch] if (f3 == 5) return a / b;
    [branch] if (f3 == 6) return zero ? a : asuint(sa % sb);
    return zero ? a : a % b;
}
// The floats that are signs, the lesser or greater, comparisons and moves (the fast loop's)
uint fp_light(uint w, uint ra, uint rb, out bool to_f, out bool ok) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7;
    precise float a = asfloat(ra), b = asfloat(rb);
    bool an = (ra & 0x7fffffff) > 0x7f800000, bn = (rb & 0x7fffffff) > 0x7f800000, less = a < b || (ra == 0x80000000 && rb == 0);
    to_f = f7 == 0x10 || f7 == 0x14 || f7 == 0x78;
    ok = f7 == 0x10 || f7 == 0x50 ? f3 < 3 : f7 == 0x14 ? f3 < 2 : ((w >> 20) & 31) == 0 && f3 == 0;
    return f7 == 0x10 ? (ra & 0x7fffffff) | ((f3 == 0 ? rb : f3 == 1 ? ~rb : ra ^ rb) & 0x80000000)
         : f7 == 0x14 ? (an ? (bn ? 0x7fc00000 : rb) : bn ? ra : ((less == (f3 == 0)) ? ra : rb))
         : f7 == 0x50 ? ((!an && !bn && (f3 == 2 ? a == b : f3 == 1 ? a < b : a <= b)) ? 1 : 0) : ra;
}
// Every other float of opcode 0x53, each kind paying for itself. The card divides by multiplying with
// 1 / b: two ordinary numbers have their exponents moved to the middle.
uint fp_all(uint w, uint ra, uint rb, out bool to_f, out bool ok) {
    uint f7 = w >> 25, f3 = (w >> 12) & 7, sel = (w >> 20) & 31;
    precise float a = asfloat(ra), b = asfloat(rb);
    // (the conversions first: Quake has more of them than of divisions and roots)
    [branch] if (f7 == 0x60) {
        bool an = (ra & 0x7fffffff) > 0x7f800000;
        float whole = f3 == 1 ? trunc(a) : f3 == 2 ? floor(a) : f3 == 3 ? ceil(a) : round(a);
        to_f = false; ok = sel < 2;
        return sel == 0 ? ((an || whole >= 2147483648.0) ? 0x7fffffff : whole <= -2147483648.0 ? 0x80000000 : asuint((int)whole))
                        : ((an || whole >= 4294967296.0) ? 0xffffffff : whole <= 0.0 ? 0 : whole >= 2147483648.0 ? 0x80000000 + (uint)(whole - 2147483648.0) : (uint)whole);
    }
    [branch] if (f7 == 0x68) {
        bool neg_in = sel == 0 && (ra >> 31) != 0;
        to_f = true; ok = sel < 2;
        return asuint((float)(neg_in ? 0 - ra : ra)) | (neg_in ? 0x80000000 : 0);
    }
    [branch] if ((f7 & 0x73) == 0 || f7 == 0x2c) {
        precise float r;
        [branch] if (f7 == 0x0c) {
            uint ea = (ra >> 23) & 0xff, eb = (rb >> 23) & 0xff;
            int de = (int)ea - (int)eb, ha = clamp(de, -126, 127);
            precise float da = asfloat((ra & 0x807fffff) | ((uint)(127 + ha) << 23)), db = asfloat((rb & 0x807fffff) | ((uint)(127 - (de - ha)) << 23));
#ifdef FAST
            r = a / b;   // (FAST: the card's division, within an ulp or two, and zero for a divisor over 2^126)
#else
            r = (ea - 1 < 254 && eb - 1 < 254) ? da / db : a / b;
#endif
        } else [branch] if (f7 == 0x2c) {
            r = sqrt(a);
        } else {
            precise float sum = a + b, dif = a - b, prod = a * b;
            r = f7 == 0 ? sum : f7 == 4 ? dif : prod;
        }
        uint ar = asuint(r);
        to_f = true; ok = f7 != 0x2c || sel == 0;
        return (ar & 0x7fffffff) > 0x7f800000 ? 0x7fc00000 : ar;
    }
    [branch] if (f7 == 0x70 && f3 == 1) {
        bool neg = (ra >> 31) != 0;
        uint man = ra & 0x7fffff, ex = (ra >> 23) & 0xff;
        to_f = false; ok = sel == 0;
        return ex == 0xff ? (man != 0 ? ((man & 0x400000) ? 0x200 : 0x100) : (neg ? 1 : 0x80))
             : ex == 0 ? (man != 0 ? (neg ? 4 : 0x20) : (neg ? 8 : 0x10)) : (neg ? 2 : 0x40);
    }
    uint light = fp_light(w, ra, rb, to_f, ok);
    ok = f7 == 0x70 ? sel == 0 && f3 == 0 : (f7 == 0x10 || f7 == 0x14 || f7 == 0x50 || f7 == 0x78) && ok;
    return light;
}

// A worker's lanes: the pixels whose eight texels are two whole buckets of its cache, texels
// 48 + 8 L to 55 + 8 L (the cache from L1_AT, buckets of four texels)
uint lanes_of(uint b) { return b < 3 ? 0 : ((L1_AT + BUCKETS(b) * 4 - 56) >> 3) + 1; }
// CSR 0x0b8 read: lanes are built (bit 31), this core's lanes (bits 8-15), how its last job ended
uint lanes_csr() { return 0x80000000u | ((core == 0 ? 0 : lanes_of(bits)) << 8) | (lane_word < 16 ? lane_word : 0); }
// CSR 0x0ba read: in a lane, the instructions left of its pass (its clock counts them from 0); elsewhere no end
uint lane_left() { uint most = max(_Ticks, 2); return lane_mode ? most - min(clock, most) : 0xffffffff; }
// The CSRs Linux uses, as fields; the machine's own read as zeros, a write to one not kept goes nowhere.
uint csr_read(uint n) {
    [branch] if (n == 0x0bf) { uint r = asuint(_Time.w); r ^= r << 13; r ^= r >> 17; r ^= r << 5; return r; }
    uint fs = (mstatus & 0x6000) != 0 ? 0x80006000 : 0;   // FS reads dirty whenever it is on
    return n == 0x301 ? 0x40141101 : n == 1 ? fcsr & 0x1f : n == 2 ? (fcsr >> 5) & 7 : n == 3 ? fcsr & 0xff :
           n == 0x100 ? (mstatus & 0xde162) | fs : n == 0x300 ? mstatus | fs : n == 0x104 ? mie & 0x222 : n == 0x304 ? mie :
           n == 0x144 ? mip & 0x222 : n == 0x344 ? mip : n == 0x105 ? stvec : n == 0x106 ? scounteren :
           n == 0x140 ? sscratch : n == 0x141 ? sepc : n == 0x142 ? scause : n == 0x143 ? stval :
           n == 0xc01 ? now_lo : n == 0xb00 || n == 0xc00 ? clock : n == 0xf14 ? core :
           n == 0x0be ? _PlayerID : n == 0x0b8 ? lanes_csr() : n == 0x0ba ? lane_left() :
           n == 0x0b9 ? (_LaneField != 0 ? (uint)FIELD_MAX : 0) :   // the field's lanes, where the host draws it
           n == 0x8b0 ? (core == 0 ? 1u : 0u) :   // a program's copy is worth asking for on the first core (docs/machine/gpu.md)
           0;
}
void csr_write(uint n, uint v) {
    // (only what interrupts see, and the copy and fill, which stall: sscratch, sepc and the rest leave the next instruction to the fast loop)
    if ((n & 0xfff) == 0x100 || n == 0x300 || (n & 0xdff) == 0x104 || (n & 0xdff) == 0x144 || (n & 0x7ff) == 0x0b0) quiet = false;
    if (n - 1 < 3) fcsr = n == 1 ? (fcsr & ~0x1fu) | (v & 0x1f) : n == 2 ? (fcsr & ~0xe0u) | ((v & 7) << 5) : v & 0xff;
    if (n == 0x100) mstatus = (mstatus & ~0xde162u) | (v & 0xde162);
    if (n == 0x300) mstatus = v;
    if (n == 0x104 || n == 0x304) mie = n == 0x304 ? v : (mie & ~0x222u) | (v & 0x222);
    if (n == 0x144 || n == 0x344) mip = n == 0x344 ? v : (mip & ~0x222u) | (v & 0x222);
    if (n == 0x105) stvec = v;
    if (n == 0x106) scounteren = v;
    if (n == 0x140) sscratch = v;
    if (n == 0x141) sepc = v;
    if (n == 0x142) scause = v;
    if (n == 0x143) stval = v;
    // the kernel's copy and fill (0x0b0-0x0b3), and a program's in the custom user range (0x8b0-0x8b3): the commit makes them
    if ((n & 0x7ff) == 0x0b1) msrc = v;
    if ((n & 0x7ff) == 0x0b2) mdst = v;
    if ((n & 0x7ff) == 0x0b3) mn = v;
    if ((n & 0x7ff) == 0x0b0) stall = v == 2 ? STALL_FILL : STALL_COPY;
    if (n == 0x0b8 && core != 0) { lane_word = v; stall = lane_mode ? STALL_LANE_FAIL : STALL_LANES_START; }   // a lanes job: v its descriptor
}
bool timer_due() { return (tcmp_lo | tcmp_hi) != 0 && (now_hi > tcmp_hi || (now_hi == tcmp_hi && now_lo >= tcmp_lo)); }
// the supervisor's calls to firmware, answered here (docs/machine/boot.md)
void sbi() {
    uint ext = xr[17], a0 = xr[10], fn = xr[16];
    if (ext == 0x54494D45 || ext == 0) { tcmp_lo = a0; tcmp_hi = xr[11]; mip = timer_due() ? mip | 0x20 : mip & ~0x20u; }
    uint val = ext != 0x10 ? 0 : fn == 0 ? 2 : fn == 1 ? 0x454d55 : fn == 2 ? 1 : (fn == 3 && (a0 == 0x54494D45 || a0 <= 2)) ? 1 : 0;
    if (ext < 0x10) xr[10] = val;
    else { xr[10] = ext == 0x10 || ext == 0x54494D45 ? 0 : 0xfffffffe; xr[11] = val; }
    quiet = false;
}
void raise(uint cause, uint val) { if (!trapped) { trapped = true; tcause = cause; tval = val; } }

// ---- The fast loop: what cannot trap or change what interrupts see, back to back; anything
// else leaves it, and the general step takes that instruction. Returns how many ran.
uint fast(uint room) {
    uint n = 0;
    budget = room;
    bool same = false;   // the last stayed within its texel: this one is the next word of it
    uint4 cur = 0;
    [loop] while (n < budget) {
        [branch] if (!same) { fetch(pc); uint s = (pc >> 2) & 3; cur = (s & 2) ? ((s & 1) ? fw0.wxyz : fw0.zwxy) : ((s & 1) ? fw0.yzwx : fw0); }
        uint w = cur.x, opc = w & 0x7f, f3 = (w >> 12) & 7, rd = (w >> 7) & 31, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31;
        uint a = xr[rs1], b = xr[rs2], next = pc + 4, v = 0, dst = rd == 0 ? 64 : rd;
        bool ok = true;
        // arithmetic and loads a group, stores and floats the next (each test passed is a level of every kind after it)
        bool alu = (w & 0x5f) == 0x13 && (w & 0xfe00007f) != 0x02000033, ld = opc == 0x03;   // (flw and fsw are the floats')
        [branch] if (alu || ld) {
            [branch] if (alu) {
                // op-imm and op: every result made, one chosen; signed is unsigned with the signs turned over.
                // Of funct7 only bit 30 is looked at (sub, sra, srai): an encoding none of our extensions has is
                // made as the one it resembles, not trapped (the test of funct7 was 2% of the shell loop)
                bool reg = (w & 0x20) != 0, alt = (w & 0x40000000) != 0;
                uint y = reg ? b : imm_i(w), s = y, turn = (f3 & 1) ? 0 : 0x80000000;   // (s: D3D's shifts take the low five bits)
                uint r01 = (f3 & 1) ? a << s : ((reg && alt) ? a - y : a + y), r23 = (a ^ turn) < (y ^ turn) ? 1 : 0;
                uint r45 = (f3 & 1) ? (alt ? asuint(asint(a) >> s) : a >> s) : a ^ y, r67 = (f3 & 1) ? a & y : a | y;
                v = (f3 & 4) ? ((f3 & 2) ? r67 : r45) : ((f3 & 2) ? r23 : r01);
                // A second word in the same turn (docs/machine/tick-costs.md, "Sixth round"): the texel's next word, when
                // it is arithmetic or a branch, its registers taken from this result where it names this one's. No test of
                // the budget: a pass may run one instruction past it, which ends it all the same (2% of Tiberian Dawn's tick)
                uint w2 = cur.y;
                [branch] if ((w2 & 0x5f) == 0x13 && ((w2 & 0xfe00007f) != 0x02000033 || (w2 & 0x7000) == 0) && (pc & 12) != 12) {   // (mul too: a product here costs a lone arithmetic word nothing)
                    uint g3 = (w2 >> 12) & 7, d2 = (w2 >> 7) & 31, s1 = (w2 >> 15) & 31, s2 = (w2 >> 20) & 31;
                    uint a2 = xr[s1], b2 = xr[s2];
                    a2 = s1 == dst ? v : a2; b2 = s2 == dst ? v : b2;
                    xr[dst] = v;
                    bool reg2 = (w2 & 0x20) != 0, alt2 = (w2 & 0x40000000) != 0;
                    uint y2 = reg2 ? b2 : imm_i(w2), turn2 = (g3 & 1) ? 0 : 0x80000000;
                    uint q01 = (g3 & 1) ? a2 << y2 : (reg2 && (w2 & 0x02000000) != 0) ? a2 * y2 : ((reg2 && alt2) ? a2 - y2 : a2 + y2), q23 = (a2 ^ turn2) < (y2 ^ turn2) ? 1 : 0;
                    uint q45 = (g3 & 1) ? (alt2 ? asuint(asint(a2) >> y2) : a2 >> y2) : a2 ^ y2, q67 = (g3 & 1) ? a2 & y2 : a2 | y2;
                    v = (g3 & 4) ? ((g3 & 2) ? q67 : q45) : ((g3 & 2) ? q23 : q01);
                    dst = d2 == 0 ? 64 : d2;
                    cur = cur.yzwx; pc += 4; next = pc + 4; n++;
                } else [branch] if ((w2 & 0x17f) == 0x63 && (pc & 12) != 12) {
                    // (a branch: its offset on a word, told by the word, as the loop's own branch tells it)
                    uint g3 = (w2 >> 12) & 7, s1 = (w2 >> 15) & 31, s2 = (w2 >> 20) & 31;
                    uint a2 = xr[s1], b2 = xr[s2];
                    a2 = s1 == dst ? v : a2; b2 = s2 == dst ? v : b2;
                    xr[dst] = v;
                    uint tb2 = (g3 & 2) ? 0 : 0x80000000;
                    bool lt2 = (a2 ^ tb2) < (b2 ^ tb2);
                    pc += 4;
                    next = (((g3 & 4) ? lt2 : a2 == b2) != ((g3 & 1) != 0)) ? pc + imm_b(w2) : pc + 4;
                    dst = 64; cur = cur.yzwx; n++;
                }
            } else {
                // lb lh lw lbu lhu, within one word of RAM: lwu is lw, and ld and funct3 7 fail the size test (no test of
                // funct3 of its own, nor of 0: the range test has it)
                uint va = a + imm_i(w), up = LOAD_UP(f3);
#ifdef FAST
                ok = (va ^ 0x80000000) < RAM_MAX;   // (FAST: a load not on its size takes its word's bytes from va on)
#else
                ok = (va & 3) + (1u << (f3 & 3)) <= 4 && (va ^ 0x80000000) < RAM_MAX;
#endif
                // (read whether or not it may be: a read changes nothing, and a load that may not is the general step's)
#ifdef FAST
                uint x = ram_word(va & 0x7ffffffc) >> ((va & 3) * 8) << up;
#else
                uint x = ram_word(va & 0x7ffffffc) << (up - ((va & 3) << 3));   // (one shift: (va & 3) * 8 is never over up for a load that may be)
#endif
                v = (f3 & 4) ? x >> up : asuint(asint(x) >> up);
                // (a branch after the load, the texel's next word, in the same turn: its registers taken from the load's word)
                uint w2 = cur.y;
                [branch] if (ok && (w2 & 0x17f) == 0x63 && (pc & 12) != 12) {
                    uint g3 = (w2 >> 12) & 7, s1 = (w2 >> 15) & 31, s2 = (w2 >> 20) & 31;
                    uint a2 = xr[s1], b2 = xr[s2];
                    a2 = s1 == dst ? v : a2; b2 = s2 == dst ? v : b2;
                    xr[dst] = v;
                    uint tb2 = (g3 & 2) ? 0 : 0x80000000;
                    bool lt2 = (a2 ^ tb2) < (b2 ^ tb2);
                    pc += 4;
                    next = (((g3 & 4) ? lt2 : a2 == b2) != ((g3 & 1) != 0)) ? pc + imm_b(w2) : pc + 4;
                    dst = 64; cur = cur.yzwx; n++;
                }
            }
        } else [branch] if (opc == 0x23 || (w & 0x5f) == 0x07 || opc == 0x53) {
            [branch] if (opc == 0x23) {
                // sb sh sw, within one word of RAM (funct3 3 to 7 fail the size test)
                uint sa = a + ((asuint(asint(w) >> 20) & 0xffffffe0) | rd), off = sa & 3;
#ifdef FAST
                ok = (sa ^ 0x80000000) < RAM_MAX;   // (FAST: a store not on its size goes into its word from sa on)
#else
                ok = (sa ^ 0x80000000) < RAM_MAX && off + (1u << f3) <= 4;
#endif
                ram_store_nc(sa & 0x7ffffffc, b << (off * 8), ok ? (0xffffffffu >> LOAD_UP(f3)) << (off * 8) : 0);   // (no test: a store that may not changes nothing)
                dst = 64;
            } else [branch] if (opc == 0x53) {
                // (opcode 0x53 before flw and fsw: a program has more of them) floats: a sum, a difference, a product, and the light ones; the rest are the general step's
                // (rs1's float register by a select, read with rs2's: under an if the second read waited for the first)
                uint f7 = w >> 25, fb = xr[32 + rs2], fa = (f7 & 0x68) != 0x68 ? xr[32 + rs1] : a;
                bool to_f = true;
#ifdef FAST
                [branch] if ((f7 & 0x73) == 0) {
                    precise float x = asfloat(fa), y = asfloat(fb), sum = x + y, dif = x - y, prod = x * y, quo = x / y;
                    v = asuint(f7 == 0 ? sum : f7 == 4 ? dif : f7 == 8 ? prod : quo);
#else
                [branch] if ((f7 & 0x73) == 0 && f7 != 0x0c) {
                    precise float x = asfloat(fa), y = asfloat(fb), sum = x + y, dif = x - y, prod = x * y;
                    v = asuint((f7 & 8) ? prod : (f7 & 4) ? dif : sum);   // (a NaN is the card's, 0x7fffffff on NVIDIA's: rvc_cpu's too)
#endif
                } else [branch] if (f7 == 0x50) {   // (comparisons by themselves before the light floats: Quake's most common float)
                    precise float x = asfloat(fa), y = asfloat(fb);
                    bool nan_ = (fa & 0x7fffffff) > 0x7f800000 || (fb & 0x7fffffff) > 0x7f800000;
                    v = !nan_ && (f3 == 2 ? x == y : f3 == 1 ? x < y : x <= y) ? 1 : 0;
                    to_f = false;
                } else [branch] if ((f7 | 8) == 0x78 && (w & 0x01f07000) == 0) {
                    v = fa;   // (fmv.w.x and fmv.x.w by themselves: through fp_light one cost a pass of them 2.36 ms, not 1.79)
                    to_f = f7 == 0x78;
                } else [branch] if (((0x50100030u >> (f7 >> 2)) & 1) != 0 && (f7 & 3) == 0 && (f7 != 0x70 || f3 == 0)) {
                    v = fp_light(w, fa, fb, to_f, ok);
                } else {
                    v = fp_all(w, fa, fb, to_f, ok);   // the divisions, roots and conversions too: a float never leaves the loop
                }
                dst = to_f ? 32 + rd : dst;
            } else [branch] if (opc == 0x07) {
                // flw, a word of RAM
                uint va = a + imm_i(w);
#ifdef FAST
                ok = f3 == 2 && (va ^ 0x80000000) < RAM_MAX;
#else
                ok = f3 == 2 && (va & 0x80000003) == 0x80000000 && (va & 0x7ffffffc) < RAM_MAX;
#endif
                v = ram_word(va & 0x7ffffffc);
                dst = 32 + rd;
            } else {
                // fsw, a word of RAM
                uint sa = a + ((asuint(asint(w) >> 20) & 0xffffffe0) | rd);
#ifdef FAST
                ok = f3 == 2 && (sa ^ 0x80000000) < RAM_MAX;
#else
                ok = f3 == 2 && (sa & 0x80000003) == 0x80000000 && (sa & 0x7ffffffc) < RAM_MAX;
#endif
                [branch] if (ok) ram_store_nc(sa & 0x7ffffffc, xr[32 + rs2], 0xffffffff);
                dst = 64;
            }
        } else [branch] if (opc == 0x63) {
            uint tb = (f3 & 2) ? 0 : 0x80000000;   // (less-than as slt's: signed is unsigned with the signs turned over)
            bool lt = (a ^ tb) < (b ^ tb);
            next = (((f3 & 4) ? lt : a == b) != ((f3 & 1) != 0)) ? pc + imm_b(w) : next;
            ok = (w & 0x100) == 0;   // (funct3 2 and 3 are beq and bne; an offset not on a word, told by the word, the general step's)
            dst = 64;
        } else [branch] if (opc == 0x37 || opc == 0x17 || opc == 0x6f || opc == 0x67) {   // (jalr's funct3 not looked at)
            bool jump = (w & 0x40) != 0;
            v = jump ? pc + 4 : (w & 0xfffff000) + (opc == 0x17 ? pc : 0);
            next = opc == 0x6f ? pc + imm_j(w) : opc == 0x67 ? a + imm_i(w) : next;
            ok = (next & 3) == 0;
        } else [branch] if (opc == 0x33 && (w >> 25) == 1) {   // (M before the atomics: Tiberian Dawn has 4.5% of mul, the atomics next to none)
            v = op_m(f3, a, b);
        } else [branch] if ((w & 0x707f) == 0x202f) {
            // amoadd amoswap lr sc amoxor amoor amoand on a word of RAM (min and max are the general step's); a funct5 under
            // 16 that is none of these is amoswap, and lr's rs2 is not looked at
            uint f5 = w >> 27, o = a & 0x7ffffffc;
#ifdef FAST
            bool lr = f5 == 2, sc = f5 == 3, sc_ok = true;   // (FAST: an sc always succeeds)
#else
            bool lr = f5 == 2, sc = f5 == 3, sc_ok = res_en != 0 && res_addr == a;
#endif
            ok = (a & 0x80000003) == 0x80000000 && o < RAM_MAX && f5 < 16;
            [branch] if (ok) {
                uint old = sc ? 0 : ram_word(o);
                [branch] if (sc ? sc_ok : !lr) ram_store_nc(o, f5 == 0 ? old + b : f5 == 4 ? old ^ b : f5 == 8 ? old | b : f5 == 12 ? old & b : b, 0xffffffff);
                res_en = lr ? 1 : sc && sc_ok ? 0 : res_en;
                res_addr = lr ? a : res_addr;
                v = sc ? (sc_ok ? 0 : 1) : old;
            }
        } else [branch] if (opc == 0x73 && (f3 & 3) != 0) {
            // csrrw, csrrs, csrrc and their immediates on a CSR nothing else looks at (sscratch, sepc, scause,
            // stval, stvec, scounteren, the float flags, time, the hart): the kernel's trap entry and exit are
            // full of them, and each was a general step
            uint cn = w >> 20, src = (f3 & 4) ? rs1 : a;
            bool wr_ = (f3 & 3) == 1 || src != 0;
            // (and sstatus while no interrupt is pending, which the kernel turns off and on around every lock: then
            // no write of it can let one in; and timeh and instret, which read 0)
            bool plain = (cn - 0x140 < 4) || cn == 0x105 || cn == 0x106 || (cn - 1 < 3) || cn == 0xc01 || cn == 0xf14 ||
                         cn == 0xc81 || cn == 0xc02 || (cn == 0x100 && (mip & mie & 0x222) == 0);
            ok = plain && ((cn >> 8) & 3) <= priv && !(wr_ && ((cn >> 10) & 3) == 3);
            [branch] if (ok) {
                v = csr_read(cn);
                if (wr_) csr_write(cn, (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? v | src : v & ~src);
            }
        } else [branch] if ((w & 0x06000073) == 0x43) {
            // fmadd, fmsub, fnmsub, fnmadd, as the general step makes them, a float of the loop's own (a program built with
            // contraction has them; rvc_cpu's fmaf rounds once, as it did before) (bit 3 the product negated, bit 2 the addend)
            float x = asfloat(xr[32 + rs1]), y = asfloat(xr[32 + rs2]), z = asfloat(xr[32 + (w >> 27)]);
            precise float p = x * y, zz = (w & 4) ? -z : z;   // (a product and a sum, each rounded: the general step's)
            uint r = asuint((w & 8) ? zz - p : p + zz);
            v = (r & 0x7fffffff) > 0x7f800000 ? 0x7fc00000 : r;
            dst = 32 + rd;
#ifdef FAST
        } else { ok = opc != 0x73 && (w & 0x73) != 0x43 && (opc != 0x0f || (f3 == 0 && w != 0x0100000f)); dst = 64; }   // (FAST: an opcode of no group is no instruction)
#else
        } else { ok = (w & 0x707f) == 0x000f && w != 0x0100000f; dst = 64; }   // (a plain fence: nothing to order)
#endif
        pre_w = w;
        if (!ok) break;
        xr[dst] = v;
        same = next == pc + 4 && (next & 12) != 0;
        cur = cur.yzwx;
        pc = next;
        n++;
    }
    pre_valid = n < budget;   // (left by a break: the word is the general step's)
    clock += n;
    return n;
}

// ---- The general step: one instruction of any kind, then the traps and interrupts.
void worker_fault();
void step() {
    clock++;
    trapped = false;
    uint w;
    [branch] if (pre_valid) w = pre_w; else w = fetch(pc);
    pre_valid = false;
    uint opc = w & 0x7f, f3 = (w >> 12) & 7, rd = (w >> 7) & 31, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31, f7 = w >> 25;
    uint a = xr[rs1], b = xr[rs2], next = pc + 4, v = 0, dst = rd == 0 ? 64 : rd;
    if ((pc & 3) != 0) { raise(0, pc); w = 0; opc = 0x7f; }
    // a read for loads and atomics, put together from two words when it is not on one (at 0: nothing)
    uint ra = (opc & 0x7b) == 0x03 ? a + imm_i(w) : (w & 0x202f) == 0x202f ? a : 0, mv = 0;
    [branch] if (ra != 0) {
        uint sh = (ra & 3) * 8;
        [loop] for (uint h = 0; h < (sh != 0 ? 2u : 1u); h++) {
            uint x = load_word((ra & ~3u) + 4 * h);
            mv |= h == 0 ? x >> sh : x << (32 - sh);
        }
    }
    [branch] if (opc == 0x03 || opc == 0x07) {
        uint up = LOAD_UP(f3), x = mv << up;
        v = (f3 & 4) ? x >> up : asuint(asint(x) >> up);
        dst = opc == 0x07 ? 32 + rd : dst;
        if (opc == 0x07 ? f3 != 2 : ((0x77u >> f3) & 1) == 0) raise(2, w);   // (lwu is lw)
    } else [branch] if (opc == 0x23 || opc == 0x27) {
        if (opc == 0x27 ? f3 != 2 : f3 > 2) raise(2, w);
        else store_any(a + imm_s(w), opc == 0x27 ? xr[32 + rs2] : b, 1u << f3);
        dst = 64;
    } else [branch] if (opc == 0x63) {
        bool lt = (f3 & 2) ? a < b : asint(a) < asint(b);
        next = (((f3 & 4) ? lt : a == b) != ((f3 & 1) != 0)) ? pc + imm_b(w) : next;
        dst = 64;
    } else [branch] if (opc == 0x37 || opc == 0x17 || opc == 0x6f || opc == 0x67) {
        v = (w & 0x40) ? pc + 4 : (w & 0xfffff000) + (opc == 0x17 ? pc : 0);
        next = opc == 0x6f ? pc + imm_j(w) : opc == 0x67 ? a + imm_i(w) : next;
    } else [branch] if (opc == 0x2f && f3 == 2) {
        // lr, sc and the atomics, on the word read above
        // (a funct5 under 16 that is none of ours is amoswap, as in the fast loop)
        uint f5 = w >> 27, x = f5 == 0 ? mv + b : f5 == 4 ? mv ^ b : f5 == 12 ? mv & b : f5 == 8 ? mv | b : f5 < 16 ? b :
                 f5 == 16 ? (asint(b) < asint(mv) ? b : mv) : f5 == 20 ? (asint(b) > asint(mv) ? b : mv) : f5 == 24 ? min(b, mv) : max(b, mv);
#ifdef FAST
        bool sc_ok = true;
#else
        bool sc_ok = res_en != 0 && res_addr == a;
#endif
        v = f5 == 3 ? (sc_ok ? 0 : 1) : mv;
        if (f5 == 2) { res_en = 1; res_addr = a; }
        if (f5 == 3 && sc_ok) res_en = 0;
        if (f5 >= 16 && (f5 & 3) != 0) raise(2, w);
        else if (f5 != 2 && (f5 != 3 || sc_ok)) store_any(a, f5 == 3 ? b : x, 4);
    } else [branch] if ((w & 0x06000073) == 0x43) {
        // fmadd, fmsub, fnmsub, fnmadd: a product and a sum, each rounded
        precise float x = asfloat(xr[32 + rs1]), y = asfloat(xr[32 + rs2]), z = asfloat(xr[32 + (w >> 27)]);
        precise float p = x * y, r = ((w >> 2) & 3) == 0 ? p + z : ((w >> 2) & 3) == 1 ? p - z : ((w >> 2) & 3) == 2 ? z - p : (-p) - z;
        v = (asuint(r) & 0x7fffffff) > 0x7f800000 ? 0x7fc00000 : asuint(r);
        dst = 32 + rd;
    } else [branch] if (opc == 0x73) {
        // every system instruction reads the CSR its top bits name, and traps where it may not
        uint n = w >> 20, src = (f3 & 4) ? rs1 : a;
        dst = 64;
        [branch] if (((n >> 8) & 3) > priv) {
            raise(2, pc);
        } else if (f3 != 0) {
            v = csr_read(n);
            dst = rd == 0 ? 64 : rd;
            [branch] if ((f3 & 3) == 1 || src != 0) {
                if (((n >> 10) & 3) == 3) raise(2, pc);
                else if (n != 0x180) csr_write(n, (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? v | src : v & ~src);
            }
        } else if (w == 0x00000073) {
            if (priv == 1) sbi(); else raise(8, pc);
        } else if (w == 0x00100073) {
            if (lane_mode) stall = pc == lane_stub ? STALL_LANE_RET : STALL_LANE_FAIL;   // a lane's index returned (to the stub)
            else if (core != 0) { mc_word = 1 << 8; stall = STALL_WFI; }   // ebreak parks a worker
        } else if (w == 0x10200073) {
            uint s = csr_read(0x100);
            csr_write(0x100, (s & ~0x20122u) | (((s >> 5) & 1) << 1) | 0x20);
            priv = (s >> 8) & 1;
            next = sepc;
        } else if (w == 0x10500073) {
            stall = STALL_WFI;
        } else if ((w & 0xfe007fff) != 0x12000073 && w != 0x30200073 && w != 0x00200073) {
            raise(2, w);
        }
    } else [branch] if (opc == 0x0f && f3 < 2) {
        // fence.i ends the pass, and pause: a worker sleeps on its job word
        dst = 64;
        if (f3 == 1) stall = STALL_FENCE;
        if (w == 0x0100000f) {
            stall = STALL_WFI;
            if (core != 0) mc_word = 3 | (TEX(RAM_ADDR((MBOX + 0x4000 + 64 * core) >> 4)).x << 8);
        }
    } else {
        raise(2, w);
    }
    if (!trapped) xr[dst] = v;
    [branch] if (core != 0 && trapped) {
        // a worker is given no trap: it stops where it is, its record in its mailbox (docs/machine/multicore.md)
        worker_fault();
        trapped = false;
        next = pc;
    }
    [branch] if (trapped) {
        // every trap is the supervisor's
        sepc = pc;
        scause = tcause;
        stval = tval;
        next = (stvec & 3) != 0 ? (stvec & ~3u) + 4 * (tcause & 0xffff) : stvec;
        mstatus = (mstatus & ~0x122u) | ((mstatus & 2) << 4) | ((priv & 1) << 8);
        priv = 1;
        quiet = false;
    }
    pc = next;
}
// After anything interrupts may see has changed: one taken if it is due (an interrupt waits while the supervisor has them
// off), and whether the fast loop may go on from here (a jump not to a word: the step traps).
void interrupts() {
    uint m = mip & mie & 0x222, pend = (m & 0x200) ? 0x200 : (m & 2) ? 2 : (m & 0x20) ? 0x20 : 0;
    [branch] if (pend != 0 && (priv != 1 || (mstatus & 2) != 0)) {
        uint cause = firstbitlow(pend);
        sepc = pc;
        scause = 0x80000000 | cause;
        stval = 0;
        pc = (stvec & 3) != 0 ? (stvec & ~3u) + 4 * cause : stvec;
        mstatus = (mstatus & ~0x122u) | ((mstatus & 2) << 4) | ((priv & 1) << 8);
        priv = 1;
        if (pend != 0x20) mip &= ~pend;   // (the timer's bit stays until the next deadline)
    }
    quiet = (pc & 3) == 0;
}
// The record of a worker's trap, the count last; a record that does not all fit is made in the next pass.
void worker_fault() {
    [branch] if (lane_mode) { stall = STALL_LANE_FAIL; return; }   // (a lane's trap: its index has failed)
    uint box = MBOX + 0x4000 + 64 * core, seen = TEX(RAM_ADDR((box >> 4) + 3)).x, call = MBOX + 0x2000 + 32 * core, k;
    bool is_call = tcause == 8, room = stall == 0;
    // a call's a7 and a0-a5, the cause, the address, pc, and the count last (one store, made in a loop;
    // each word worked out where it is stored: no array beside the registers and the cache)
    uint n = is_call ? 7 : 0;
    [loop] for (k = 0; k < n + 4; k++) {
        if (k == n + 3 && stall != 0) break;
        uint j = k - n;
        uint a = k < n ? call + 4 * k : box + (j == 3 ? 32 : 36 + 4 * j);
        uint v = k < n ? (k == 0 ? xr[17] : xr[9 + k]) : j == 0 ? tcause : j == 1 ? tval : j == 2 ? pc : seen + 1;
        ram_store(a, v, 0xffffffff);
    }
    if (stall == 0) { mc_word = (is_call ? 9 : 5) | (seen << 8); stall = STALL_WFI; }
    else if (room) ovf_addr = 0xffffffff;
}

// ---- The pass
void load_state() {
    uint4 t0 = st(0), t1 = st(1), t2 = st(2), t8 = st(8), t28 = st(28), t37 = st(37);
    uint k;
    mstatus = t0.x; mie = t0.y; mip = t0.z; stvec = t0.w;
    sscratch = t1.x; sepc = t1.y; scause = t1.z; stval = t1.w;
    scounteren = t2.x; fcsr = t2.y;
    priv = t8.z; ovf_addr = 0xffffffff;
    tcmp_lo = st(10).y; tcmp_hi = st(10).z;
    stall = t28.x; clock = t28.y; lane_word = t28.w;
    // (one loop for both, pc's word then put right: two loops and no second read of f0 were 2.4 to 3.1% slower on Quake)
    [loop] for (k = 0; k < 8; k++) {
        uint4 r = st(29 + k), f = st(FP_AT + k);
        xr[4 * k + 1] = r.x; xr[4 * k + 2] = r.y; xr[4 * k + 3] = r.z; xr[4 * k + 4] = r.w;
        xr[32 + 4 * k] = f.x; xr[33 + 4 * k] = f.y; xr[34 + 4 * k] = f.z; xr[35 + 4 * k] = f.w;
    }
    xr[0] = 0; pc = st(36).w; xr[32] = st(FP_AT).x;   // (texel 36's last word is pc, not f0)
    res_en = t37.x; res_addr = t37.y; msrc = t37.z; mdst = t37.w; mn = st(38).z;
    mc_word = st(41).w;
    now_lo = (_MtimeLoLo & 0xffff) | (_MtimeLoHi << 16); now_hi = 0;
    if (timer_due()) mip |= 0x20;
}
// a worker's start, stop or answer, from its mailbox as the last commit left it
void worker_enter() {
    bool called = (mc_word & 9) == 9;
    if (mc_word & 14) mc_word = 1;
    uint4 box = TEX(RAM_ADDR((MBOX >> 4) + core));
    if (box.x == MC_START) {
        pc = box.y; xr[2] = box.z; xr[10] = core; priv = 0; stall = 0; mc_word = 1;
    } else if (box.x == MC_STOP || (mc_word & 1) == 0) {
        if (mc_word & 1) mc_word = 2 << 8;
        stall = STALL_WFI;
    } else if (called) {
        xr[10] = TEX(RAM_ADDR(((MBOX + 0x4000 + 64 * core) >> 4) + 3)).y;
        pc += 4;
    }
}
// whether the selected worker has nothing to do in this pass (its pixels then keep what they hold)
bool worker_idle() {
    uint start = TEX(RAM_ADDR((MBOX >> 4) + core)).x, word = st(41).w, box = (MBOX + 0x4000 + 64 * core) >> 4;
    if (start == MC_START || (start == MC_STOP && (word & 1) != 0)) return false;
    if ((word & 1) == 0) return true;
    if (word & 12) return (TEX(RAM_ADDR(box + 3)).x & 0xffffff) == (word >> 8);
    return (word & 2) != 0 && (TEX(RAM_ADDR(box)).x & 0xffffff) == (word >> 8);
}
// texel lin of the state after this pass
uint4 state_out(uint lin) {
    uint end = L1_AT + BUCKETS(bits) * 4;
    [branch] if (lin >= L1_AT && lin < end) {
        uint off = lin - L1_AT, bk = off >> 2;
        if (!OCC(bk)) return 0;
        uint4 tags = c[4 * bk];
        return (off & 3) == 0 ? tags : idx(tags, (off & 3) - 1) == 0 ? (uint4)0 : c[off];
    }
    if (lin >= FP_AT) return uint4(xr[32 + 4 * (lin - FP_AT)], xr[33 + 4 * (lin - FP_AT)], xr[34 + 4 * (lin - FP_AT)], xr[35 + 4 * (lin - FP_AT)]);
    if (lin >= 29 && lin < 37) return uint4(xr[4 * (lin - 29) + 1], xr[4 * (lin - 29) + 2], xr[4 * (lin - 29) + 3], lin == 36 ? pc : xr[4 * (lin - 29) + 4]);
    uint4 was = st(lin);
    return lin == 0 ? uint4(mstatus, mie, mip, stvec) : lin == 1 ? uint4(sscratch, sepc, scause, stval) : lin == 2 ? uint4(scounteren, fcsr, 0, 0) :
           lin == 8 ? uint4(ovf_addr, ovf_val, priv, 0) : lin == 10 ? uint4(0, tcmp_lo, tcmp_hi, 0) : lin == 11 ? uint4(0, 0, 0, 0xffffffff) :
           lin == 28 ? uint4(stall, clock, was.z, lane_word) : lin == 37 ? uint4(res_en, res_addr, msrc, mdst) : lin == 38 ? uint4(0, 0, mn, 0) :
           lin == 40 ? uint4(0, 0, stall, 0) : lin == 41 ? uint4(bloom, dirty, was.z, mc_word) : lin < 41 ? (uint4)0 : was;
}

// ---- Lanes (docs/machine/lanes.md). A job is a function, an index for each call of it, and an
// output of whole texels an index; lane L of n makes indices first + L, first + L + n, ... in its
// order, as many in a pass as fit its six texels and its instructions, from a stack and a cache of
// its own (the pixel's local array). What it made goes into its two buckets, which the commit
// writes as it writes any core's; the last word of each bucket's tags is the lane's place in its
// order and its instructions in the pass, read by every pixel of the worker in the next pass.

// The lane's next index: its registers as a call of the function with the index and the context
void lane_go() {
    xr[10] = lane_first + lane_L + lanes_n * ((lane_j & 0x9fffffff) + lane_k); xr[11] = lane_ctx; xr[2] = lane_sp; xr[1] = lane_stub;
    pc = lane_fn; stall = 0; quiet = true; pre_valid = false; trapped = false;
}
// A lanes pass, before anything runs: the job's descriptor (three texels of RAM: the function, the
// first index, the count, the output; texels an index, the stack's top, the context, the stub; where
// a stuck lane says so), the clock moved on by the lanes' instructions of the pass before, and the
// job through (the worker goes on in this pass) or not (this pixel a lane, or one that runs nothing).
void lanes_begin(uint w0) {
    uint end = L1_AT + BUCKETS(bits) * 4, d = (lane_word & 0x7fffffff) >> 4, L, sum = 0;
    uint4 d0 = TEX(RAM_ADDR(d)), d1 = TEX(RAM_ADDR(d + 1));
    bool start = stall == STALL_LANES_START, all = true, stuck = false, more = false;
    lanes_n = lanes_of(bits);
    lane_fn = d0.x; lane_first = d0.y; lane_count = d0.z; lane_out = (d0.w & 0x7fffffff) >> 4;
    lane_T = clamp(d1.x, 1, 6); lane_sp = d1.y; lane_ctx = d1.z; lane_stub = d1.w;
    lane_status = TEX(RAM_ADDR(d + 2)).x;
    [loop] for (L = 0; L < lanes_n && !start; L++) {
        uint cur = st(48 + 8 * L).w;
        sum += st(52 + 8 * L).w;
        bool gone = (cur >> 31) != 0;
        stuck = stuck || (cur >> 30) != 0;
        more = more || ((cur >> 29) & 1) != 0;
        all = all && (gone || L + lanes_n * (cur & 0x1fffffff) >= lane_count);
    }
    clock += sum;
    if (start) all = lane_count == 0;
    if (all) { stall = 0; lane_word = (stuck ? 2 : 1) | (more ? 4 : 0); return; }
    stall = STALL_LANES; bloom = 0xffffffff; dirty = 0xffffffff;
    lane_pixel = w0 >= 48 && w0 + 8 <= end;
    if (!lane_pixel) return;
    lane_L = (w0 - 48) >> 3;
    lane_j = start ? 0 : st(w0).w;
    lane_k = 0; lane_ran = 0; lane_used = 0; lane_stuck = false;
    if ((lane_j >> 31) != 0 || lane_L + lanes_n * (lane_j & 0x9fffffff) >= lane_count) return;
    lane_mode = true; mie = 0; bits = 6;   // (the whole local array its cache)
    clock = 0;   // (a lane pixel writes no clock: from 0, its instructions in the pass, CSR 0x0ba)
    lane_go();
}
// After a lane's run ended at n instructions of `most`: whether it goes on with its next index
// (an index that returned, room in its texels, and instructions for another by those made so far).
// An index takes the texels it wrote; one that does not fit them is made again in the next pass.
bool lane_next(uint n, uint most) {
    lane_ran = n;
    [branch] if (stall == STALL_LANE_RET) {
        uint t0 = lane_out + lane_T * (lane_L + lanes_n * ((lane_j & 0x9fffffff) + lane_k)), q, w = 0;
        [loop] for (q = 0; q < lane_T; q++) w += find(t0 + q) != 0 ? 1u : 0u;
        if (lane_used + w > 6) return false;
        lane_used += w;
        if ((lane_status & 1) != 0 && xr[10] != 0) lane_j |= 0x20000000;   // (a job in steps, bit 0 of its stuck texels' address: this one not through)
        lane_k++;
        if (lane_used < 6 && lane_L + lanes_n * ((lane_j & 0x9fffffff) + lane_k) < lane_count && (most - n) * lane_k * 2 >= n) { lane_go(); return true; }
    } else {
        lane_stuck = lane_k == 0;   // (its first in a whole pass did not end: it never will here)
    }
    return false;
}
// entry e of the lane's six into its buckets: texel t's tag and what it holds
void lane_put(inout uint e, uint t, uint4 v) {
    uint tg = e < 3 ? 0 : 4;
    ot[e < 3 ? 1 + e : 2 + e] = v;
    ot[tg] = put(ot[tg], e < 3 ? e : e - 3, t + 1);
    e++;
}
// A lane pixel's eight texels: the output texels of the indices it made, as its cache has them
// (one an index never stored to is as RAM has it), its place in its order and its instructions
void lane_output() {
    uint e = 0, i, q, j = lane_j & 0x9fffffff;
    for (i = 0; i < 8; i++) ot[i] = 0;
    [loop] for (i = 0; i < lane_k && e < lane_used; i++) {
        uint t0 = lane_out + lane_T * (lane_L + lanes_n * (j + i));
        [loop] for (q = 0; q < lane_T; q++) {
            uint at = find(t0 + q);
            [branch] if (at != 0) lane_put(e, t0 + q, c[at]);
        }
    }
    // (stuck: one of the first 96 K of its order is skipped, its bit set in the lane's stuck texels,
    // and the lane goes on; one after them stops it there, said in its last. A texel's first word is
    // the job's: one of another job is taken for empty)
    bool skip = false;
    [branch] if (lane_stuck && lane_status != 0) {
        // (K, the stuck texels a lane has but its last: the descriptor's, read again here and not kept
        // through the loop; the field's lanes are no core's)
        uint d = ((core == 0xffff ? TEX(RAM_ADDR(FIELD_JOB_T)).x : lane_word) & 0x7fffffff) >> 4;
        uint K = clamp(TEX(RAM_ADDR(d + 2)).w, 1, 64);
        skip = j < 96 * K;
        uint q = ((j >> 5) * 0xAAABu) >> 17;   // (j / 96: j is under 96 K where it is used)
        uint at = ((lane_status & 0x7fffffff) >> 4) + lane_L * (K + 1) + (skip ? q : K), b = j - 96 * q;
        uint4 s = TEX(RAM_ADDR(at));
        if (s.x != (0x80000000u | lane_job)) s = uint4(0x80000000u | lane_job, 0, 0, 0);
        if (!skip) s.y = 0x80000000u | j;
        else if (b < 32) s.y |= 1u << b;
        else if (b < 64) s.z |= 1u << (b - 32);
        else s.w |= 1u << (b - 64);
        lane_put(e, at, s);
    }
    ot[0].w = (skip ? (j + 1) | 0x40000000u : lane_stuck ? 0x80000000u | j : (j + lane_k) | (lane_j & 0x40000000)) | (lane_j & 0x20000000);
    ot[4].w = lane_ran;
}

// ---- The field's lanes. Lane pixel F's two tag texels keep its place in its order (A's last word)
// and the job's number in the top byte of B's, its instructions for the job in sixteens below it.
static uint field_F, field_seq, field_N, field_cum;
static uint4 field_was_a, field_was_b, field_status;
static bool field_on = false, field_lead = false;
// texel w of the field's state (lane F's are 8 F to 8 F + 7)
uint4 field_st(uint w) {
    uint F = w >> 3, k = 8 + (F >> 4), i = F & 15;
    return TEX(uint2(((k & 31) << 6) + 40 + ((i >> 3) << 3) + (w & 7), ((k >> 5) << 3) + (i & 7)));
}
// Pixel F of the field before anything runs: a leader looks at its lanes; a lane with indices left
// of the job posted is set to run its first, its registers those of a call from nowhere (the
// program's global and thread pointers, every other register 0).
void field_begin(uint F) {
    uint4 job = TEX(RAM_ADDR(FIELD_JOB_T));
    uint N = min(job.z, (uint)FIELD_MAX), groups = (N + 63) >> 6, i;
    field_F = F; field_N = N; field_seq = job.y & 0xff; lane_job = job.y;
    core = 0xffff; ovf_addr = 0xffffffff; stall = STALL_LANES;   // (no core's: a trap is the lane's)
    lane_k = 0; lane_ran = 0; lane_used = 0; lane_stuck = false;
    field_was_a = field_st(8 * F); field_was_b = field_st(8 * F + 4);
    bool same = (field_was_b.w >> 24) == field_seq;
    lane_j = same ? field_was_a.w : 0;
    field_cum = same ? field_was_b.w & 0xffffff : 0;
    field_lead = (F & 63) == 0;
    field_on = job.x != 0 && F < N;
    if (!field_on) return;
    uint d = (job.x & 0x7fffffff) >> 4;
    uint4 d0 = TEX(RAM_ADDR(d)), d1 = TEX(RAM_ADDR(d + 1)), d2 = TEX(RAM_ADDR(d + 2));
    lanes_n = N - groups;
    lane_fn = d0.x; lane_first = d0.y; lane_count = d0.z; lane_out = (d0.w & 0x7fffffff) >> 4;
    lane_T = clamp(d1.x, 1, 6); lane_sp = d1.y; lane_ctx = d1.z; lane_stub = d1.w; lane_status = d2.x;
    lane_L = F - (F >> 6) - 1;
    [branch] if (field_lead) {
        // how its 63 lanes left the last pass: all through this job's order, stuck, and their instructions
        // (and which of them left indices to the first core: lanes 0 to 30 in bits 1 to 31 of the second
        // word, 31 to 62 in the fourth)
        bool busy = false, more = false;
        uint sum = 0, m0 = 0, m1 = 0;
        [loop] for (i = 1; i < 64 && F + i < N; i++) {
            uint ai = field_st(8 * (F + i)).w, bi = field_st(8 * (F + i) + 4).w, li = F + i - (F >> 6) - 1;
            bool here = (bi >> 24) == field_seq;
            busy = busy || !here || ((ai >> 31) == 0 && li + lanes_n * (ai & 0x1fffffff) < lane_count);
            [flatten] if (here && (ai >> 30) != 0) { if (i <= 31) m0 |= 1u << i; else m1 |= 1u << (i - 32); }
            more = more || (here && ((ai >> 29) & 1) != 0);
            sum += here ? bi & 0xffffff : 0;
        }
        field_status = uint4(job.y, (busy ? 1u : 0u) | m0, sum | (more ? 0x80000000u : 0u), m1);   // (bit 31: an index not through)
        return;
    }
    if ((lane_j >> 31) != 0 || lane_L + lanes_n * (lane_j & 0x9fffffff) >= lane_count) return;
    xr[3] = d2.y; xr[4] = d2.z;
    priv = 0; mie = 0; mip = 0; fcsr = 0; res_en = 0; mstatus = 0x6000;
    lane_mode = true; bits = 6;
    lane_go();
}
// Pixel F's eight texels: a leader's group word, a lane's output and place, or as they were
void field_output() {
    uint e = 0, i;
    [branch] if (!field_on) {
        for (i = 0; i < 8; i++) ot[i] = 0;
        ot[0].w = field_was_a.w; ot[4].w = field_was_b.w;
        return;
    }
    [branch] if (field_lead) {
        for (i = 0; i < 8; i++) ot[i] = 0;
        lane_put(e, FIELD_STATUS_T + (field_F >> 6), field_status);
        ot[4].w = field_seq << 24;
        return;
    }
    lane_output();
    ot[4].w = (field_seq << 24) | ((field_cum + (lane_ran >> 4)) & 0xffffff);
}

struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) { v2f o; o.vertex = float4(0, 0, 0, 1); return o; }
#ifndef TICK_UNPACK
// one triangle over all the targets: frag discards what no core runs
[maxvertexcount(3)]
void tick_geom(triangle v2f IN[3], uint prim : SV_PrimitiveID, inout TriangleStream<v2f> s) {
#ifdef TICK_GEOM_GUARD
    TICK_GEOM_GUARD
#endif
    if (prim == 0) {
        v2f o;
        o.vertex = float4(-1, 1, 0, 1); s.Append(o);
        o.vertex = float4(3, 1, 0, 1); s.Append(o);
        o.vertex = float4(-1, -3, 0, 1); s.Append(o);
        s.RestartStrip();
    }
}

struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    bool field = false;
    [branch] if (p.y >= 8) {
        // a pixel of the field: lane F, while a job is posted, as many rows as its lanes fill
        uint4 fjob = TEX(RAM_ADDR(FIELD_JOB_T));
        if (_Init || _LaneField == 0 || fjob.x == 0 || p.y - 8 >= (min(fjob.z, (uint)FIELD_MAX) + 2047) >> 11) { discard; }
        field = true;
        field_F = ((p.y - 8) << 11) + p.x;
    }
    geo_read();
    select_core(p.x >> 3);
    uint k, px = p.x & 7;
    uint w0 = core == 0 ? p.y * 64 + px * 8 : (px << 6) + (p.y << 3);   // the pixel's first texel, in its core's numbering
    uint2 own = uint2(((core & 31) << 6) + px * 8, ((core >> 5) << 3) + p.y);   // and where that is in the state texture
    if (!field && core != 0 && (_Init != 0 || core > g_count || px >= rows_now || worker_idle())) discard;
    bool any = field;
    for (k = 0; k < 8; k++) any = any || lin_of(w0 + k) < FP_AT + 8;
    [branch] if (any) {
        if (_Init) {
            // as firmware would enter the kernel: supervisor mode, the device tree's address in a1
            stall = 0; clock = 0; priv = 1; pc = 0x82000000; ovf_addr = 0xffffffff;
            xr[11] = 0x83e00000;
        } else {
            [branch] if (field) {
                field_begin(field_F);
            } else {
                load_state();
                if (core != 0) worker_enter();
                [branch] if (core != 0 && (stall == STALL_LANES || stall == STALL_LANES_START)) lanes_begin(w0);
            }
            uint n = 0, most = max(_Ticks, 2);
            // (one loop for a core and for a lane, whose indices are its runs one after another)
            [loop] for (;;) {
                [loop] while (n < most && stall == 0) {
                    if (!quiet) interrupts();   // (no [branch]: 0.2% of the shell loop)
                    [branch] if (quiet) n += fast(most - n);
                    if (n >= most || stall != 0) break;
                    step();
                    n++;
                }
                if (!lane_mode) break;   // (two tests: HLSL makes both sides of an ||, and lane_next changes what it looks at)
                if (!lane_next(n, most)) break;
            }
        }
    }
    [branch] if (field) field_output();
    else [branch] if (lane_pixel) lane_output();
    else for (k = 0; k < 8; k++) {
        uint lin = lin_of(w0 + k);
        ot[k] = lin >= FP_AT + 8 ? TEX(own + uint2(k, 0)) : state_out(lin);
    }
    tick_out r;
    r.t0 = ot[0]; r.t1 = ot[1]; r.t2 = ot[2]; r.t3 = ot[3]; r.t4 = ot[4]; r.t5 = ot[5]; r.t6 = ot[6]; r.t7 = ot[7];
    return r;
}
#endif
