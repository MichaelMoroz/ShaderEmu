#!/usr/bin/env python3
"""The tick builder: a gpu_vm tick (a D3D11 pixel shader, gpu_vm/tick/STATE.md's contract) composed from a point of a
parameter space. A variant is a dict of parameters; emit(P) returns plain HLSL (no switches left in it) and manifest(P)
the parameters with what they make (the state's size, the layout's constants gvmbench reads).

    python gpu_vm/tick/builder/gen.py OUT.hlsl [name=value ...]      (one variant; prints its manifest)

The fixed parts (parts/*.hlsl) are the ISA's semantics, which the reference (gpu_vm/ref/gvm_ref.h) decides and which have
one right answer: the integer and float units, the branch test, step12 (paths 1 and 2), the general step with traps,
special registers and interrupts, the commit and the operations. Everything that only decides how fast it runs is a
parameter here (README.md, "The space").
"""
import json, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))

# x23's design (gpu_vm/tick/steps/README.md "x23") is the default point
DEFAULTS = dict(
    # ---- the write cache
    nb=32,              # buckets of four ways a table (two tables): 16, 32, 64
    bloom=1,            # a load whose texel has a bit no stored texel had skips the tags
    hash=0,             # the buckets' hashes: 0 x23's (xor-folds), 1 a multiplicative pair
    clear='loop',       # the tags' clearing at the pass's start: loop, unroll
    # ---- the fast loop
    fetch='both',       # the next bundle: 'both' texels (fall-through and target) then a select; 'one': the branch first, one texel
    int_first=0,        # the integer path tested before the memory paths
    mem_order='35',     # which memory path is tested first: '35' (one op and lite), '4' (two loads)
    split='branch',     # the integer/memory split: branch or flatten
    # ---- the pass: where a core's state pixels lie (core c, state pixel j)
    bw=8, bh=8,         # a core's block, pixels: bw * bh >= the state's pixels
    band=16,            # rows from one band of blocks to the next (>= bh)
    per=128,            # blocks a band (a power of two)
    jrow=0,             # pixel j in its block: 0 column by column (x23), 1 row by row
    perm='id',          # core -> block within a band: id, bitrev, mul
    mul=1,              # perm mul's odd factor
    early=0,            # 0 none; 1 a halted core copies its state at the top; 2 and a sleeping one (a supervisor's wait);
                        # 3 and both discard when the other set holds the same (texel 23.w counts the quiet passes)
    # ---- phase 2 (ISA): a large register file's words in the state after the write cache (0: none; not yet run)
    xregs=0,
)

SPACE = dict(   # the values a search draws from
    nb=[16, 32, 64], bloom=[0, 1], hash=[0, 1], clear=['loop', 'unroll'],
    fetch=['both', 'one'], int_first=[0, 1], mem_order=['35', '4'], split=['branch', 'flatten'],
    block=[(8, 8), (16, 4), (4, 16), (8, 4), (4, 8), (32, 2), (16, 8), (8, 16), (32, 4), (64, 1), (32, 1), (16, 2)],
    bandx=[1, 2], per=[16, 32, 64, 128], jrow=[0, 1], perm=['id', 'bitrev', 'mul'], mul=[3, 5, 37, 77],
    early=[0, 1, 2, 3],
)

def layout(P):
    nb = P['nb']
    wcn = 8 * nb + 2
    t_wc = 40
    t_wt = t_wc + wcn
    ntex = t_wt + (wcn + 3) // 4
    t_xr = ntex
    ntex += (P['xregs'] + 3) // 4
    spix = (ntex + 7) // 8
    return dict(WCN=wcn, T_WC=t_wc, T_WT=t_wt, NTEX=ntex, SPIX=spix, T_XR=t_xr)

def valid(P):
    """'' when the point can be built, else why not"""
    L = layout(P)
    if P['bw'] * P['bh'] < L['SPIX']: return 'block %dx%d < %d state pixels' % (P['bw'], P['bh'], L['SPIX'])
    if P['band'] < P['bh']: return 'band < bh'
    if P['per'] & (P['per'] - 1): return 'per not a power of two'
    if P['per'] * P['bw'] > 16384: return 'too wide'
    if P['nb'] & (P['nb'] - 1): return 'nb not a power of two'
    if P['perm'] == 'mul' and P['mul'] % 2 == 0: return 'mul even'
    if P['xregs']: return 'xregs: phase 2, not built yet'
    return ''

def inv_mod(m, n):
    return pow(m, -1, n) if n > 1 else 0

def part(name):
    with open(os.path.join(HERE, 'parts', name + '.hlsl')) as f:
        return f.read()

def header(P, L):
    per, logp = P['per'], int(math.log2(P['per']))
    perm = {'id': 0, 'bitrev': 1, 'mul': 2}[P['perm']]
    mul = P['mul'] % per if per > 1 else 0
    inv = inv_mod(mul, per) if perm == 2 else 0
    if perm == 0 or per == 1:
        slot = 'uint slot_of(uint i) { return i; }\nuint core_at(uint s) { return s; }'
    elif perm == 1:
        slot = ('uint slot_of(uint i) { return reversebits(i) >> %d; }\nuint core_at(uint s) { return reversebits(s) >> %d; }'
                % (32 - logp, 32 - logp))
    else:
        slot = ('uint slot_of(uint i) { return (i * %du) & %du; }\nuint core_at(uint s) { return (s * %du) & %du; }'
                % (mul, per - 1, inv, per - 1))
    jx = '(j %% %du)' % P['bw'] if P['jrow'] else '(j / %du)' % P['bh']
    jy = '(j / %du)' % P['bw'] if P['jrow'] else '(j %% %du)' % P['bh']
    isa = os.path.normpath(os.path.join(HERE, '..', '..', 'isa.h')).replace(os.sep, '/')
    import hashlib
    ISA, ISAH = isa, hashlib.sha1(open(isa, 'rb').read()).hexdigest()[:12]
    return f'''// A tick made by gpu_vm/tick/builder/gen.py (the manifest beside it has its parameters); not edited by hand.
#include "{ISA}"   // isa.h {ISAH}

static const uint NB = {P['nb']};           // buckets of four entries a table
static const uint WCN = {L['WCN']};         // write cache entries: 8 * NB in buckets, two overflow slots, then a sink
static const uint WCS = WCN - 2;
static const uint NONE = 0xffffffffu;
static const uint RAM_W = 8192;
static const uint T_WC = {L['T_WC']};
static const uint T_Q = 24;
static const uint T_WT = T_WC + WCN;
static const uint NTEX = {L['NTEX']};
static const uint SPIX = {L['SPIX']};
// the layout, as gvmbench reads it: core c's state pixel j at (slot * GEO_BW + jx, band * GEO_BAND + jy), its band c / GEO_PER,
// its slot in the band GEO_PERM's (0 the same, 1 bits reversed, 2 times GEO_MUL), jx, jy by GEO_JROW
static const uint GEO_BLOCKS = 1;
static const uint GEO_BW = {P['bw']};
static const uint GEO_BH = {P['bh']};
static const uint GEO_BAND = {P['band']};
static const uint GEO_PER = {per};
static const uint GEO_JROW = {P['jrow']};
static const uint GEO_PERM = {perm};
static const uint GEO_MUL = {mul};
static const uint GEO_INV = {inv};
{slot}
int3 sat(uint j, uint c) {{ return int3(slot_of(c % {per}u) * {P['bw']}u + {jx}, (c / {per}u) * {P['band']}u + {jy}, 0); }}
static const uint QMAX = 8;
static const uint OP_SLOTS = 32;
static const uint OP_SPAN = 272;
static const uint F_WAIT = 1, F_SOFT = 2, F_HALT = 4, F_SYNC = 8, F_AGAIN = 16;
static const uint E_BUDGET = 0, E_SYNC = 1, E_WAIT = 2, E_FULL = 3, E_HALT = 5;
Texture2D<uint4> _Ram : register(t0), _Rom : register(t1);
Texture2D<uint4> _S0 : register(t2), _S1 : register(t3), _S2 : register(t4), _S3 : register(t5);
Texture2D<uint4> _S4 : register(t6), _S5 : register(t7), _S6 : register(t8), _S7 : register(t9);
Texture2D<uint4> _OpState : register(t10), _Stage : register(t11);
cbuffer PassParams : register(b0) {{
    uint _Ticks, _RamEnd, _TimeLo, _TimeHi, _Player, _Flags, _RamRows, _Ext0, _NCores; uint3 _ppad; uint4 _Ext[2];
}};
uint4 ram(uint t) {{ return _Ram.Load(int3(t % RAM_W, t / RAM_W, 0)); }}
uint4 data_texel(uint ad) {{
    uint t = ad >> 4;
    int3 at = int3(t % RAM_W, (t / RAM_W) & 0x1fff, 0);
    uint4 rom = _Rom.Load(at), ram = _Ram.Load(at);
    return (ad & GVM_ROM) != 0 ? rom : ram;
}}
uint pick(uint4 q, uint i) {{ return (i & 2) ? ((i & 1) ? q.w : q.z) : ((i & 1) ? q.y : q.x); }}
uint4 put(uint4 v, uint i, uint x) {{ return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }}
static uint r[65];
static uint wt[WCN + 1];
static uint wd[4 * (WCN + 1)];
static uint bloom;
static uint pc, tp, cycle, opsrun, flags, ops8;
static uint4 cur;
static uint thr;
static uint lmax, smax;
static uint st, epc, cause, tval, tvec, scratch, ien, tcmp_lo, tcmp_hi, rnd, left, core, ext, endr;
static uint4 lat0, lat1;
static uint4 q[16];
static uint qn, qhead, qgen, refused;
static bool irq_due;
'''

def memory(P):
    lg = int(math.log2(P['nb']))
    if P['hash'] == 0:
        h = '''uint wc_b1(uint t) { return (t ^ (t >> 5) ^ (t >> 10)) & (NB - 1); }
uint wc_b2(uint t) { return NB + (((t >> 3) ^ (t << 2) ^ (t >> 5)) & (NB - 1)); }'''
    else:
        h = f'''uint wc_b1(uint t) {{ return (t * 0x9e3779b1u) >> {32 - lg}; }}
uint wc_b2(uint t) {{ return NB + ((t * 0x85ebca6bu + 0x165667b1u) >> {32 - lg}); }}'''
    look = "st || ((ad >> 4) & bloom) == (ad >> 4)" if P['bloom'] else 'true'
    return '''// ---- Memory: the write cache holds a texel in an entry of its bucket in the first table or the second
bool has(uint tg, uint t, uint i) { return tg != NONE && (tg & 0x0fffffffu) == t && ((tg >> (28 + i)) & 1) != 0; }
uint stag(uint tg, uint t, uint i) { return t | (((tg == NONE ? 0u : tg >> 28) | (1u << i)) << 28); }
uint pick_ad(uint4 q, uint ad) { uint2 h = (ad & 8) ? q.zw : q.xy; return (ad & 4) ? h.y : h.x; }
uint mem_addr(uint w, uint imm, uint a, uint b) { return a + ((w & op_bit(16)) ? b << (w >> 20) : imm); }
uint ext_k(uint w) { return (w >> 9) & 24; }
uint extend(uint w, uint word, uint ad) {
    uint k = ext_k(w), x = (word >> (ad << 3)) << k;
    return (w & op_bit(4)) ? x >> k : asuint(asint(x) >> k);
}
uint merge(uint w, uint word, uint ad, uint val) {
    uint sh = ad << 3, msk = (0xffffffffu >> ext_k(w)) << sh;
    return word ^ ((word ^ (val << sh)) & msk);
}
''' + h + '''
uint4 bucket(uint b) { uint k = 4 * b; return uint4(wt[k], wt[k + 1], wt[k + 2], wt[k + 3]); }
struct mop { uint ad, t, i, b1, b2; uint4 q, A, B; uint sl, otag, e, w; };
void mfind(inout mop m) {
    bool4 ka = (m.A & 0x0fffffffu) == m.t || m.A == NONE, kb = (m.B & 0x0fffffffu) == m.t || m.B == NONE;
    uint msk = (ka.x ? 1u : 0u) | (ka.y ? 2u : 0u) | (ka.z ? 4u : 0u) | (ka.w ? 8u : 0u)
             | (kb.x ? 16u : 0u) | (kb.y ? 32u : 0u) | (kb.z ? 64u : 0u) | (kb.w ? 128u : 0u);
    uint k = firstbitlow(msk);
    m.otag = k < 8 ? pick(k < 4 ? m.A : m.B, k & 3) : NONE;
    m.sl = k < 4 ? 4 * m.b1 + k : k < 8 ? 4 * m.b2 + (k & 3) : NONE;
    m.e = has(m.otag, m.t, m.i) ? m.sl : WCN;
}
mop mlook(uint ad, bool look) {
    mop m;
    m.ad = ad; m.t = ad >> 4; m.i = (ad >> 2) & 3; m.b1 = wc_b1(m.t); m.b2 = wc_b2(m.t);
    m.q = 0; m.A = NONE; m.B = NONE;
    [branch] if (look) {
        m.A = bucket(m.b1); m.B = bucket(m.b2);
    }
    mfind(m);
    m.w = wd[4 * m.e + m.i];
    return m;
}
mop mread_entries(uint w, uint imm, uint a, uint b, bool st) {
    uint ad = mem_addr(w, imm, a, b);
    return mlook(ad, ''' + look + ''');
}
mop mread_load(uint w, uint imm, uint a, uint b) {
    mop m = mread_entries(w, imm, a, b, false);
    m.q = data_texel(m.ad);
    return m;
}
mop mread(uint w, uint imm, uint a, uint b) {
    mop m = mread_entries(w, imm, a, b, true);
    m.q = data_texel(m.ad);
    return m;
}
uint seen(mop m) { return m.e != WCN ? m.w : pick(m.q, m.i); }
uint slot(mop m) { return m.sl; }
uint slot_tag(mop m, uint sl) { return m.otag; }
'''

MEM35 = '''[branch] if ((path & 1) != 0) {   // a load or a store, and a lite op (paths 3 and 5)
            bool st0 = (t.x & op_bit(8)) != 0;
            mop m0 = mread_entries(t.x, i0, A0, B0, st0);
            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);
            smax = max(smax, st0 ? m0.ad - GVM_DEV : 0); lmax = max(lmax, st0 ? 0 : m0.ad & ~GVM_ROM);
            uint w0 = seen(m0), sl0 = slot(m0);
            v0 = extend(t.x, w0, m0.ad);
            bool nr = st0 && sl0 == NONE;
            uint at0 = st0 ? (nr ? WCS : sl0) : WCN;
            wt[at0] = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wd[4 * at0 + m0.i] = merge(t.x, w0, m0.ad, r[d0]);
            bloom |= st0 ? m0.t : 0;
            cycle -= nr ? budget - 1 : 0; budget = nr ? 1 : budget;
            s0 = st0 ? 64 : s0;
            r[s0] = v0;
            v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
        }'''
MEM4 = '''[branch] if (path == GVM_PATH_LOAD2) {   // two loads
            mop m0 = mread_load(t.x, i0, A0, B0), m1 = mread_load(t.y, i1, A1, B1);
            lmax = max(lmax, max(m0.ad & ~GVM_ROM, m1.ad & ~GVM_ROM));
            v0 = extend(t.x, seen(m0), m0.ad);
            r[s0] = v0;
            v1 = extend(t.y, seen(m1), m1.ad);
        }'''
MEM6 = '''{   // a store, then a load or a store that sees it by selects (path 6)
            bool st1 = (t.y & op_bit(8)) != 0;
            mop m0 = mread_entries(t.x, i0, A0, B0, true), m1 = mread_entries(t.y, i1, A1, B1, true);
            [branch] if (((t.x & op_bit(3)) | ((t.y ^ op_bit(8)) & op_bit(11)) | ((m0.ad | m1.ad) & 3)) != 0) { m0.q = data_texel(m0.ad); m1.q = data_texel(m1.ad); }
            smax = max(smax, max(m0.ad - GVM_DEV, st1 ? m1.ad - GVM_DEV : 0)); lmax = max(lmax, st1 ? 0 : m1.ad & ~GVM_ROM);
            uint w0 = seen(m0), sl0 = slot(m0), s0x = sl0 == NONE ? WCS : sl0;
            uint tg0 = stag(slot_tag(m0, sl0), m0.t, m0.i);
            uint e0 = merge(t.x, w0, m0.ad, r[d0]);
            bool same = m0.t == m1.t, over = same && sl0 == NONE;
            wt[s0x] = tg0; wd[4 * s0x + m0.i] = e0;
            m1.A = (s0x >> 2) == m1.b1 ? put(m1.A, s0x & 3, tg0) : m1.A;
            m1.B = (s0x >> 2) == m1.b2 ? put(m1.B, s0x & 3, tg0) : m1.B;
            mfind(m1);
            m1.sl = over ? WCS : m1.sl; m1.otag = over ? tg0 : m1.otag; m1.e = over ? (has(tg0, m1.t, m1.i) ? WCS : WCN) : m1.e;
            m1.w = wd[4 * m1.e + m1.i];
            uint w1 = seen(m1), sl1 = m1.sl;
            v1 = extend(t.y, w1, m1.ad);
            bool nr = sl0 == NONE || (st1 && sl1 == NONE);
            uint o1 = m1.otag;
            uint at1 = st1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN;
            wt[at1] = stag(o1, m1.t, m1.i);
            wd[4 * at1 + m1.i] = merge(t.y, w1, m1.ad, r[d1]);
            bloom |= m0.t | (st1 ? m1.t : 0);
            cycle -= nr ? budget - 1 : 0; budget = nr ? 1 : budget;
            s1 = st1 ? 64 : s1;
        }'''
INT = '''{   // integers: op 0 the mid unit, op 1 lite
        v0 = alu_mid(t.x, i0, A0, B0);
        r[s0] = v0;
        v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
    }'''

def fstep(P):
    if P['mem_order'] == '35':
        mem = MEM35 + ' else ' + MEM4 + ' else ' + MEM6
    else:
        mem = MEM4 + ' else ' + MEM35 + ' else ' + MEM6
    mem = '{\n        ' + mem + '\n    }'
    attr = '[%s] ' % P['split']
    if P['int_first']:
        split = attr + 'if (path == GVM_PATH_INT) ' + INT + ' else ' + mem
    else:
        split = attr + 'if (path != GVM_PATH_INT) ' + mem + ' else ' + INT
    if P['fetch'] == 'both':
        pre = '    uint4 nx = ram(tn), tc = ram(tg);\n'
        post = '    tp = tk ? tg : tn;\n    cur = tk ? tc : nx;\n'
        tkpre = ''
        tkpost = '    uint tk = taken(ctl, CA, CB, i6, sum);\n'
    else:
        pre = ''
        tkpre = ('    uint i6 = asuint((int)ctl >> 26), sum = CA + i6;\n    uint tk = taken(ctl, CA, CB, i6, sum);\n'
                 '    uint tnext = tk ? tg : tn;\n    uint4 nxt = ram(tnext);\n')
        post = '    tp = tnext;\n    cur = nxt;\n'
        tkpost = ''
    i6line = '    uint i6 = asuint((int)ctl >> 26), sum = CA + i6;\n' if P['fetch'] == 'both' else ''
    return '''// ---- The fast loop's step: paths 3-7
void fstep(inout uint budget) {
    uint4 t = cur;
    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18;
    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;
''' + pre + '''    uint i0 = (ctl & 0x20) ? t.z : simm12(t.x), i1 = (ctl & 0x40) ? t.z : simm12(t.y);
    uint d0 = (t.x >> 6) & 63, d1 = (t.y >> 6) & 63, s0 = d0 != 0 ? d0 : 64, s1 = d1 != 0 ? d1 : 64;
    uint ra0 = t.x & 63, rb0 = t.x >> 26, ra1 = t.y & 63, rb1 = t.y >> 26;
    uint ca = (ctl >> 11) & 63, cb = ctl >> 26;
    uint A0 = r[ra0], B0 = r[rb0], A1 = r[ra1], B1 = r[rb1], CA = r[ca], CB = r[cb];
''' + tkpre + '''    uint v0, v1;
    ''' + split + '''
    r[s1] = v1;
''' + i6line + tkpost + '''    r[(ctl & 0x780) != (GVM_CC_ADDBNZ << 7) ? 64 : ca] = sum;
''' + post + '''    budget--;
    ops8 += n8;
}
'''

RUN = '''void run(uint budget) {
    [branch] if (flags & F_HALT) { endr = E_HALT; return; }
    [branch] if ((flags & F_SYNC) != 0 && qn != 0) { endr = E_SYNC; return; }
    flags &= ~F_SYNC;
    [branch] if ((flags & F_WAIT) != 0 && (ipend() & ien) == 0) { endr = E_WAIT; return; }
    flags &= ~F_WAIT;
    irq_due = (st & GVM_ST_IE) != 0 && (ipend() & ien) != 0;
    cur = ram(pc >> 4);
    cur.w &= (pc & 15) != 0 || irq_due ? ~7u : 0xffffffffu;
    uint guard = budget + 8;   // a hard cap on the trips of both loops, whatever the budget does
    [loop] while (budget > 0 && guard > 0) {
        uint b0 = budget;
        guard--;
        tp = pc >> 4;
        ops8 = 0;
        [loop] while (budget > 0 && guard > 0 && (cur.w & 7) >= thr) { fstep(budget); guard--; }
        cycle += b0 - budget;
        opsrun += ops8 >> 3;
        pc = b0 != budget ? tp << 4 : pc;
        uint path = cur.w & 7;
        [branch] if (budget > 0 && (path == GVM_PATH_ALU || path == GVM_PATH_MEMF) && thr == GVM_PATH_FAST) budget = step12(budget);
        else [branch] if (budget > 0) budget = general(budget);
    }
}
'''

def state_io(P):
    clear = ('[loop] for (k = 0; k <= WCN; k++) wt[k] = NONE;' if P['clear'] == 'loop'
             else '[unroll] for (k = 0; k <= WCN; k++) wt[k] = NONE;')
    return '''uint4 state_in(uint t, uint c) {
    int3 at = sat(t >> 3, c);
    switch (t & 7) {
    case 0: return _S0.Load(at); case 1: return _S1.Load(at); case 2: return _S2.Load(at); case 3: return _S3.Load(at);
    case 4: return _S4.Load(at); case 5: return _S5.Load(at); case 6: return _S6.Load(at); default: return _S7.Load(at);
    }
}
uint4 state_out(uint t, uint4 s3) {
    [branch] if (t < 16) return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);
    [branch] if (t >= T_WC && t < T_WT) { uint e = 4 * (t - T_WC); return uint4(wd[e], wd[e + 1], wd[e + 2], wd[e + 3]); }
    [branch] if (t >= T_WT && t < T_WT + (WCN + 3) / 4) {
        uint k = 4 * (t - T_WT);
        return uint4(wt[k], wt[k + 1], k + 2 < WCN ? wt[k + 2] : NONE, k + 3 < WCN ? wt[k + 3] : NONE);
    }
    [branch] if (t >= T_Q && t < T_WC) return q[t - T_Q];
    uint4 s16 = uint4(pc, st, epc, cause), s17 = uint4(tval, tvec, scratch, ien), s18 = uint4(tcmp_lo, tcmp_hi, flags, rnd);
    uint4 s19 = uint4(cycle, opsrun, cycle - s3.x, endr == E_BUDGET && cycle - s3.x < _Ticks && (flags & F_HALT) == 0 ? E_FULL : endr);
    uint4 s22 = uint4(qn, qhead, qgen, refused);
    return t == 16 ? s16 : t == 17 ? s17 : t == 18 ? s18 : t == 19 ? s19 : t == 20 ? lat0 : t == 21 ? lat1 : t == 22 ? s22 : 0;
}
uint4 again_out(uint t, uint c) {
    uint4 v = state_in(t, c);
    return t >= T_WT ? uint4(NONE, NONE, NONE, NONE) : t >= T_WC ? uint4(0, 0, 0, 0) : t == 18 ? uint4(v.xy, v.z | F_AGAIN, v.w) : t == 19 ? uint4(v.xyz, E_BUDGET) : v;
}
struct v2f { float4 vertex : SV_POSITION; };
v2f vert(uint id : SV_VertexID) {
    v2f o;
    o.vertex = float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1);
    return o;
}
struct tick_out { uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                  uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7; };
void load_state(uint c) {
    uint k;
    [unroll] for (k = 0; k < 16; k++) {
        uint4 s = state_in(k, c);
        r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;
    }
    r[0] = 0;
    ''' + clear + '''
    bloom = 0;
    uint4 s16 = state_in(16, c), s17 = state_in(17, c), s18 = state_in(18, c), s19 = state_in(19, c);
    uint4 s22 = state_in(22, c), os = _OpState.Load(int3(c, 0, 0));
    pc = s16.x; st = s16.y; epc = s16.z; cause = s16.w;
    tval = s17.x; tvec = s17.y; scratch = s17.z; ien = s17.w;
    tcmp_lo = s18.x; tcmp_hi = s18.y; flags = s18.z; rnd = s18.w;
    cycle = s19.x; opsrun = s19.y; endr = E_BUDGET;
    lat0 = state_in(20, c); lat1 = state_in(21, c);
    qn = min(s22.x, QMAX); qhead = s22.y; qgen = s22.z; refused = s22.w;
    [unroll] for (k = 0; k < 16; k++) q[k] = state_in(T_Q + k, c);
    [branch] if (os.x == qgen) {
        uint d = min(os.y, qn);
        [loop] for (k = 0; k + d < qn; k++) { q[2 * k] = q[2 * (k + d)]; q[2 * k + 1] = q[2 * (k + d) + 1]; }
        qn -= d; qhead = os.z; refused += os.w;
    }
    qgen++;
}
'''

def frag(P):
    bw, bh, band, per = P['bw'], P['bh'], P['band'], P['per']
    j = ('ry * %du + rx' % bw) if P['jrow'] else ('rx * %du + ry' % bh)
    early = ''
    if P['early']:
        wake = ''
        if P['early'] >= 2:
            wake = '''        else [branch] if ((s18.z & F_WAIT) != 0) {   // asleep (a supervisor's wait) with no interrupt pending and enabled
            bool due = (s18.x | s18.y) != 0 && (_TimeHi > s18.y || (_TimeHi == s18.y && _TimeLo >= s18.x));
            uint ip = ((s18.z & F_SOFT) ? 1u : 0u) | (due ? 2u : 0u) | (ext << 2);
            [flatten] if ((ip & state_in(17, core).w) == 0) why = E_WAIT;
        }
'''
        disc = ''
        if P['early'] >= 3:
            disc = '''        [branch] if (s23.w >= 2) discard;   // the other set holds this state too (quiet in the last two passes)
'''
        early = '''    {   // a core that runs nothing this pass: its state as it was, the write cache empty (no other work)
        uint4 s18 = state_in(18, core), s22 = state_in(22, core);
        uint why = NONE;
        [branch] if (s22.x == 0) {
            [branch] if ((s18.z & F_HALT) != 0) why = E_HALT;
''' + wake + '''        }
        [branch] if (why != NONE) {
            uint4 s23 = state_in(23, core);
''' + disc + '''            uint4 os = _OpState.Load(int3(core, 0, 0));
            bool fold = os.x == s22.z;
            uint4 n22 = uint4(0, fold ? os.z : s22.y, s22.z + 1, s22.w + (fold ? os.w : 0));
            uint4 n23 = uint4(0, 0, 0, min(s23.w + 1, 2u));
            uint4 v[8];
            [unroll] for (uint k = 0; k < 8; k++) {
                uint t = j * 8 + k;
                uint4 x = t < T_WC ? state_in(t, core) : t < T_WT ? uint4(0, 0, 0, 0) : t < T_WT + (WCN + 3) / 4 ? uint4(NONE, NONE, NONE, NONE) : uint4(0, 0, 0, 0);
                x = t == 18 ? uint4(x.xy, x.z & ~F_AGAIN & (why == E_WAIT ? ~F_SYNC : 0xffffffffu), x.w) : t == 19 ? uint4(x.xy, 0, why) : t == 22 ? n22 : t == 23 ? n23 : x;
                v[k] = x;
            }
            o.t0 = v[0]; o.t1 = v[1]; o.t2 = v[2]; o.t3 = v[3]; o.t4 = v[4]; o.t5 = v[5]; o.t6 = v[6]; o.t7 = v[7];
            return o;
        }
    }
'''
    return '''tick_out frag(v2f i) {
    uint2 p = uint2(i.vertex.xy);
    tick_out o;
    uint rx = p.x % ''' + str(bw) + '''u, ry = p.y % ''' + str(band) + '''u;
    core = (p.y / ''' + str(band) + 'u) * ' + str(per) + 'u + core_at(p.x / ' + str(bw) + '''u);
    uint j = ''' + j + ''';
    [branch] if (core >= _NCores || j >= SPIX || ry >= ''' + str(bh) + '''u) { o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = 0; return o; }
    ext = (pick(_Ext[core >> 7], (core >> 5) & 3) >> (core & 31)) & 1;
''' + early + '''    load_state(core);
    thr = (flags & F_AGAIN) != 0 || qn != 0 ? GVM_PATH_INT : GVM_PATH_FAST;
    flags &= ~F_AGAIN;
    lmax = 0; smax = 0;
    run(_Ticks);
    bool again = lmax >= _RamEnd || smax >= _RamEnd - GVM_DEV;
    uint4 s19 = state_in(19, core);
    o.t0 = state_out(j * 8 + 0, s19); o.t1 = state_out(j * 8 + 1, s19); o.t2 = state_out(j * 8 + 2, s19); o.t3 = state_out(j * 8 + 3, s19);
    o.t4 = state_out(j * 8 + 4, s19); o.t5 = state_out(j * 8 + 5, s19); o.t6 = state_out(j * 8 + 6, s19); o.t7 = state_out(j * 8 + 7, s19);
    [branch] if (again) {
        uint t = j * 8;
        o.t0 = again_out(t, core); o.t1 = again_out(t + 1, core); o.t2 = again_out(t + 2, core); o.t3 = again_out(t + 3, core);
        o.t4 = again_out(t + 4, core); o.t5 = again_out(t + 5, core); o.t6 = again_out(t + 6, core); o.t7 = again_out(t + 7, core);
    }
    return o;
}
'''

def full(P):
    P = dict(DEFAULTS, **P)
    return P

def emit(P):
    P = full(P)
    why = valid(P)
    if why: raise ValueError(why)
    L = layout(P)
    out = [header(P, L), part('alu'), memory(P), part('taken'), fstep(P), part('step12'), part('general'), RUN,
           state_io(P), frag(P), part('ops')]
    return '\n'.join(out)

def manifest(P):
    P = full(P)
    return dict(params=P, layout=layout(P))

def parse_kv(args):
    P = {}
    for a in args:
        k, v = a.split('=', 1)
        d = DEFAULTS[k]
        P[k] = int(v) if isinstance(d, int) else v
    return P

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    P = parse_kv(sys.argv[2:])
    src = emit(P)
    with open(sys.argv[1], 'w', newline='\n') as f: f.write(src)
    m = manifest(P)
    with open(os.path.splitext(sys.argv[1])[0] + '.json', 'w') as f: json.dump(m, f, indent=1)
    print(json.dumps(m))
