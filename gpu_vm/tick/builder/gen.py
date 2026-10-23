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
    mru=0,              # x24's idea: the last N stored texels' entries in registers (0, 1, 2, 4): a hit reads no tags
    bb=0,               # x24's idea: a mask of the first-table buckets stored to; a load whose first bucket has none reads no tags
    tfirst=0,           # x24's idea: a load's or store's texel fetched before its lookup (paths 3-5)
    s6=0,               # x24's idea (with mru): a store pair (path 6) whose texels are both kept skips the lookup
    nc=0,               # phase 2: GVM_W_NC loads (read-only data) skip the write cache's lookup
    thin=0,             # one op a bundle: the fast loop drops op 1 (the assembler closes a bundle after every op)
    thin8=0,            # phase 3: 8-byte instructions {op, control} (the assembler's thin8), one op each
    wide3=0,            # phase 3: three ops a bundle (op 2 lite, in word 2): 1 behind a branch, 2 always made
    s12b=0,             # step12's op 0 integer or float: 0 a select of both, 1 a branch; 2 op 1's too
    fx1=0,              # phase 3: op 1 takes any float function but the special ones (compares, sign, conversions; gvm_asm.h fx1)
    late=0,             # phase 3: a conditional branch reads its registers after the bundle's ops (no op-less bundle before it)
    vec=0,              # phase 3: 128-bit registers and the vector ops (isa.h GVM_X_VADD ...; rpk=wide); 2: lanes read at first use
    rc=0,               # a read cache: the last 0, 1 or 2 texels the fast paths fetched, in registers
    dtx='both',         # a data texel: ROM and RAM both fetched then selected (x23), or one behind a branch
    rpk='scalar',       # the registers: scalar r[65], or vec uint4 R4[17] (layoutopts.py)
    pk='scalar',        # the write cache: scalar wt[] and wd[], vec (uint4 tags a bucket, uint4 data an entry), aos (a bucket's tags and texels side by side)
    lay='split',        # the arrays apart (split) or one array (merged)
    early=0,            # 0 none; 1 a halted core copies its state at the top; 2 and a sleeping one (a supervisor's wait);
                        # 3 and both discard when the other set holds the same (texel 23.w counts the quiet passes)
    # ---- phase 2 (ISA; isa.h GVM_M_FREAD): the core's file, its bindings and its pair buffer, in the state after the write cache
    xr=0,               # the file's words (0: none; 64 to 1024)
    npair=0,            # pairs a pass (0: none; even)
    xbind=1,            # slots may be bound (a tag word a slot; the commit writes the dirty ones)
    xload='pre',        # the file into local memory: 'pre' whole at the pass's start, 'lazy' a texel at its first use
    xfast=1,            # lf, sf and fpair in the fast loop's paths 3 and 5 (the assembler's GvmAsm::file_paths)
)

SPACE = dict(   # the values a search draws from
    nb=[16, 32, 64], bloom=[0, 1], hash=[0, 1], clear=['loop', 'unroll'],
    fetch=['both', 'one'], int_first=[0, 1], mem_order=['35', '4'], split=['branch', 'flatten'],
    block=[(8, 8), (16, 4), (4, 16), (8, 4), (4, 8), (32, 2), (16, 8), (8, 16), (32, 4), (64, 1), (32, 1), (16, 2)],
    bandx=[1, 2], per=[16, 32, 64, 128], jrow=[0, 1], perm=['id', 'bitrev', 'mul'], mul=[3, 5, 37, 77],
    early=[0, 1, 2, 3], mru=[0, 1, 2, 4], bb=[0, 1], tfirst=[0, 1], s6=[0, 1], nc=[0, 1], thin=[0, 1], thin8=[0, 1], wide3=[0, 1, 2], late=[0, 1], fx1=[0, 1], s12b=[0, 1, 2], rc=[0, 1, 2], dtx=['both', 'one'], rpk=['scalar', 'vec', 'wide'], pk=['scalar', 'vec', 'aos'], lay=['split', 'merged'],
    xr=[0, 64, 128, 256, 512, 1024], npair=[0, 32, 64, 128], xbind=[0, 1], xload=['pre', 'lazy'], xfast=[0, 1],
)

def layout(P):
    nb = P['nb']
    wcn = 8 * nb + 2
    t_wc = 40
    t_wt = t_wc + wcn
    ntex = t_wt + (wcn + 3) // 4
    xr, np_, xb = P['xr'], P['npair'], P['xbind'] if P['xr'] else 0
    t_xf = ntex
    t_xt = t_xf + xr // 4
    t_pb = t_xt + (xr // 4 if xb else 0)
    ntex = t_pb + np_ // 2
    t_vr = ntex   # phase 3: the 128-bit registers, a texel each (lane 0 also in texels 0-15)
    ntex += 64 if P.get('vec', 0) else 0
    spix = (ntex + 7) // 8
    cpts = wcn + (xr if xb else 0) + np_
    return dict(WCN=wcn, T_WC=t_wc, T_WT=t_wt, NTEX=ntex, SPIX=spix, T_XF=t_xf, T_XT=t_xt, T_PB=t_pb, CPTS=cpts, XBIND=xb, T_VR=t_vr)

def valid(P):
    """'' when the point can be built, else why not"""
    L = layout(P)
    if P['bw'] * P['bh'] < L['SPIX']: return 'block %dx%d < %d state pixels' % (P['bw'], P['bh'], L['SPIX'])
    if P['band'] < P['bh']: return 'band < bh'
    if P['per'] & (P['per'] - 1): return 'per not a power of two'
    if P['per'] * P['bw'] > 16384: return 'too wide'
    if P['nb'] & (P['nb'] - 1): return 'nb not a power of two'
    if P['perm'] == 'mul' and P['mul'] % 2 == 0: return 'mul even'
    if P['xr'] and (P['xr'] & (P['xr'] - 1) or P['xr'] < 4): return 'xr not a power of two'
    if P['npair'] % 2: return 'npair odd'
    if P.get('s6', 0) and not P.get('mru', 0): return 's6 without mru'
    if P.get('late', 0) and P.get('wide3', 0): return 'late with wide3'
    if P.get('vec', 0) and P.get('rpk', 'scalar') != 'wide': return 'vec needs rpk=wide'
    if P.get('wide3', 0) and (P.get('thin', 0) or P.get('thin8', 0)): return 'wide3 with thin'
    if P.get('thin8', 0) and (P['fetch'] != 'both' or P.get('mru', 0)): return 'thin8 with fetch=both only'
    if P['xr'] > 1024 or P['npair'] > 256: return 'file or pairs too large'
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
// the core's file (phase 2): XR words from texel T_XF, their tags from T_XT (XBIND), NPAIR pairs from T_PB (their count
// texel 23.x); the commit draws CPTS points a core: the write cache's, the file's slots (XBIND), the pairs
static const uint XR = {P['xr']};
static const uint XBIND = {L['XBIND']};
static const uint XFAST = {P['xfast'] if P['xr'] or P['npair'] else 0};
static const uint NCLOAD = {P.get('nc', 0)};   // GVM_W_NC loads (no look at this pass's stores)
static const uint THIN = {P.get('thin', 0)};   // one op a bundle (the assembler's max_ops 1): the fast loop has no op 1
static const uint THIN8 = {P.get('thin8', 0)};   // 8-byte instructions (gvm_machine.thin)
static const uint WIDE3 = {P.get('wide3', 0)};   // three ops a bundle (gvm_machine.wide3)
static const uint FX1 = {P.get('fx1', 0)};   // op 1's float functions: fpu's but the special ones (GvmAsm::fx1)
static const uint LATEBR = {P.get('late', 0)};   // branches read their registers after the bundle's ops (gvm_machine.late)
static const uint XLAZY = {1 if P['xload'] == 'lazy' else 0};
static const uint NPAIR = {P['npair']};
static const uint T_XF = {L['T_XF']};
static const uint T_XT = {L['T_XT']};
static const uint T_PB = {L['T_PB']};
static const uint T_VR = {L['T_VR']};
static const uint VEC = {P.get('vec', 0)};   // 128-bit registers (gvm_machine.vec; needs rpk=wide)
static const uint CPTS = {L['CPTS']};
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
''' + xstatics(P)

def xstatics(P):
    """the core's file (phase 2): its local copies and their access"""
    xr, np_ = P['xr'], P['npair']
    xb = P['xbind'] if xr else 0
    lazy = P['xload'] == 'lazy'
    o = "static uint pn;   // this pass's pairs\n"
    if np_:
        o += "static uint4 pa4[NPAIR / 2];   // (address, value) twice a texel\n"
        o += "void xpair(uint ad, uint v) { uint k = pn >> 1; pa4[k] = (pn & 1) ? uint4(pa4[k].xy, ad, v) : uint4(ad, v, pa4[k].zw); pn++; }\n"
    if not xr: return o
    o += "static uint4 xf4[XR / 4];\n"
    if xb: o += "static uint4 xt4[XR / 4];   // a slot's tag: the word's address | 1 bound | 2 dirty\n"
    T = ''
    if lazy:
        o += "static uint xl[(XR / 4 + 31) / 32];   // the file's texels read in this pass\n"
        o += "uint4 state_in(uint t, uint c);\n"
        o += "bool xin(uint t) { return ((xl[t >> 5] >> (t & 31)) & 1) != 0; }\n"
        o += "void xtouch(uint t) {\n    [branch] if (!xin(t)) {\n        xl[t >> 5] |= 1u << (t & 31);\n        xf4[t] = state_in(T_XF + t, core);\n"
        if xb: o += "        xt4[t] = state_in(T_XT + t, core) & ~2u;\n"
        o += "    }\n}\n"
        T = "    xtouch(i >> 2);\n"
    o += "uint xget(uint i) {\n" + T + "    return pick(xf4[i >> 2], i & 3);\n}\n"
    o += "void xput(uint i, uint v) {\n" + T + "    xf4[i >> 2] = put(xf4[i >> 2], i & 3, v);\n"
    if xb: o += "    uint g = pick(xt4[i >> 2], i & 3);\n    xt4[i >> 2] = put(xt4[i >> 2], i & 3, (g & 1) != 0 ? g | 2u : g);\n"
    o += "}\n"
    if xb:
        o += "uint xgtag(uint i) {\n" + T + "    return pick(xt4[i >> 2], i & 3);\n}\n"
        o += "void xstag(uint i, uint v) {\n" + T + "    xt4[i >> 2] = put(xt4[i >> 2], i & 3, v);\n}\n"
        o += "void xsetv(uint i, uint v) {\n" + T + "    xf4[i >> 2] = put(xf4[i >> 2], i & 3, v);\n}\n"
    return o

FILE_PAIR = """    [branch] if (pair) {
        uint ad = (a + imm) & ~3u;
        [branch] if (!store_ok(ad, 4) || ad >= GVM_OPS) { trap(GVM_CAUSE_STORE, epcw, ad); return 1; }
        [branch] if (op_waits(ad, 4, true)) { pc = epcw; e = E_SYNC; return 3; }
        [branch] if (pn >= NPAIR) { pc = epcw; e = E_FULL; return 3; }
        xpair(ad, r[rd]);
        return 0;
    }
"""
FILE_BIND = """    [branch] if ((xgtag(i) & 2) != 0) { pc = epcw; e = E_FULL; return 3; }   // a dirty slot: the commit writes it first
    [branch] if (mode == GVM_M_FWRITE) { xstag(i, 0); return 0; }
    uint ad = r[rd] & ~3u;
    [branch] if (ad < GVM_DEV || ad >= _RamEnd) { trap(GVM_CAUSE_LOAD, epcw, ad); return 1; }
    [branch] if (op_waits(ad, 4, false)) { pc = epcw; e = E_SYNC; return 3; }
    xsetv(i, word_seen(ad));
    xstag(i, ad | 1);
"""

def file_any(P):
    """the general step's file ops (isa.h GVM_M_FREAD; the reference's gvm__op): 0 done, 1 trapped, 3 the pass ends before it"""
    xr, np_ = P['xr'], P['npair']
    xb = P['xbind'] if xr else 0
    o = "uint file_any(uint w, uint imm, uint a, uint epcw, inout uint e, inout bool ill) {\n"
    o += "    uint opc = (w >> 12) & 255, mode = (opc >> 3) & 7, k = opc & 7, rd = (w >> 6) & 63;\n"
    o += "    bool pair = mode == GVM_M_FWRITE && k == GVM_FK_PAIR;\n"
    o += "    ill = pair ? NPAIR == 0 : XR == 0 || (mode == GVM_M_FREAD ? k > GVM_FK_BIND : k > GVM_FK_UNBIND)%s;\n" % (
        '' if xb else ' || (mode == GVM_M_FREAD ? k == GVM_FK_BIND : k == GVM_FK_UNBIND)')
    o += "    [branch] if (ill) return 0;\n"
    if np_: o += FILE_PAIR
    if xr:
        o += "    uint i = (a + imm) & (XR - 1);\n"
        o += "    [branch] if (mode == GVM_M_FREAD && k == GVM_FK_LF) { r[rd == 0 ? 64 : rd] = xget(i); return 0; }\n"
        o += "    [branch] if (mode == GVM_M_FWRITE && k == GVM_FK_SF) { xput(i, r[rd]); return 0; }\n"
        if xb: o += FILE_BIND
    o += "    return 0;\n}\n"
    return o

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

def xfast(P):
    """paths 3 and 5 with op 0 the file's lf, sf or fpair (the assembler's file_paths), before the memory op's body"""
    xr, np_ = P['xr'], P['npair']
    o = "[branch] if ((t.x & op_bit(0x30)) == op_bit(0x30)) {   // the core's file: lf, sf, fpair\n"
    o += "            bool wr = (t.x & op_bit(8)) != 0;\n            v0 = 0;\n"
    if xr:
        o += "            uint xi = (A0 + i0) & (XR - 1);\n"
        o += "            [branch] if (!wr) v0 = xget(xi);\n"
        o += "            else [branch] if (((t.x >> 12) & 7) == GVM_FK_SF) xput(xi, r[d0]);\n"
        o += "            else {\n" if np_ else ''
    else:
        o += "            {\n"
    if np_:
        o += """                uint pad = (A0 + i0) & ~3u;
                smax = max(smax, pad - GVM_DEV);
                xpair(pad, r[d0]);
                bool nr = pn >= NPAIR;
                cycle -= nr ? budget - 1 : 0; budget = nr ? 1 : budget;
            }
"""
    elif not xr:
        o += "            }\n"
    o += """            s0 = wr ? 64 : s0;
            r[s0] = v0;
            v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
        } else """
    return o

def fstep(P):
    m35 = (xfast(P) if (P['xr'] or P['npair']) and P['xfast'] else '') + MEM35
    if P['mem_order'] == '35':
        mem = m35 + ' else ' + MEM4 + ' else ' + MEM6
    else:
        mem = MEM4 + ' else ' + m35 + ' else ' + MEM6
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

def xout(P):
    o = ''
    if P['xr']:
        lz = P['xload'] == 'lazy'
        o += "    [branch] if (t >= T_XF && t < T_XF + XR / 4) { uint k = t - T_XF; return %s; }@N" % ('xin(k) ? xf4[k] : state_in(t, core)' if lz else 'xf4[k]')
        if P['xbind']:
            o += "    [branch] if (t >= T_XT && t < T_XT + XR / 4) { uint k = t - T_XT; return %s; }@N" % ('xin(k) ? xt4[k] : state_in(t, core) & ~2u' if lz else 'xt4[k]')
    if P['npair']:
        o += "    [branch] if (t >= T_PB && t < T_PB + NPAIR / 2) return pa4[t - T_PB];@N"
    return o.replace('@N', chr(10))

def xload(P):
    if not P['xr']: return ''
    if P['xload'] == 'lazy': return "    [unroll] for (k = 0; k < (XR / 4 + 31) / 32; k++) xl[k] = 0;" + chr(10)
    o = "    [loop] for (k = 0; k < XR / 4; k++) {@N        xf4[k] = state_in(T_XF + k, c);@N"
    if P['xbind']: o += "        xt4[k] = state_in(T_XT + k, c) & ~2u;@N"
    return (o + "    }@N").replace('@N', chr(10))

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
''' + xout(P) + '''    [branch] if (t >= T_Q && t < T_WC) return q[t - T_Q];
    uint4 s16 = uint4(pc, st, epc, cause), s17 = uint4(tval, tvec, scratch, ien), s18 = uint4(tcmp_lo, tcmp_hi, flags, rnd);
    uint4 s19 = uint4(cycle, opsrun, cycle - s3.x, endr == E_BUDGET && cycle - s3.x < _Ticks && (flags & F_HALT) == 0 ? E_FULL : endr);
    uint4 s22 = uint4(qn, qhead, qgen, refused);
    return t == 16 ? s16 : t == 17 ? s17 : t == 18 ? s18 : t == 19 ? s19 : t == 20 ? lat0 : t == 21 ? lat1 : t == 22 ? s22 : t == 23 ? uint4(pn, 0, 0, 0) : 0;
}
uint4 again_out(uint t, uint c) {   // (the file as it was, its slots clean: their values were committed; no pairs)
    uint4 v = state_in(t, c);
    [branch] if (t >= T_XF && t < T_XT) return v;
    [branch] if (t >= T_XT && t < T_PB) return v & ~2u;
    [branch] if (t >= T_PB || t == 23) return 0;
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
    pn = 0;
''' + xload(P) + '''    uint4 s16 = state_in(16, c), s17 = state_in(17, c), s18 = state_in(18, c), s19 = state_in(19, c);
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
                uint4 x = t < T_WC ? state_in(t, core) : t < T_WT ? uint4(0, 0, 0, 0) : t < T_XF ? uint4(NONE, NONE, NONE, NONE)
                        : t < T_XT ? state_in(t, core) : t < T_PB ? state_in(t, core) & ~2u : uint4(0, 0, 0, 0);
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
    if P.get('fit', 1):   # a block too small for the state grows: columns first, then rows (band kept >= rows)
        while P['bw'] * P['bh'] < layout(P)['SPIX'] and P['bw'] * P['bh'] < 1024:
            if P['bw'] <= P['bh'] * 2: P['bw'] *= 2
            else: P['bh'] *= 2
            P['band'] = max(P['band'], P['bh'])
    return P

def step12(P):
    """step12 (paths 1 and 2): with the assembler's file_paths a file op with a cc 0 control word (call, ret, jr) is path 2's"""
    src = part('step12')
    if not ((P['xr'] or P['npair']) and P['xfast']): return src
    xr, np_ = P['xr'], P['npair']
    o = "    [branch] if (path == GVM_PATH_MEMF && (t.x & op_bit(0x30)) == op_bit(0x30)) {   // the core's file: lf, sf, fpair\n"
    o += "        bool wr = (t.x & op_bit(8)) != 0;\n"
    if xr:
        o += "        uint xi = (A0 + i0) & (XR - 1);\n"
        o += "        [branch] if (!wr) v0 = xget(xi);\n"
        o += "        else [branch] if (((t.x >> 12) & 7) == GVM_FK_SF) xput(xi, r[d0]);\n"
        if np_: o += "        else {\n"
    else:
        o += "        {\n"
    if np_:
        o += """            uint pad = (A0 + i0) & ~3u;
            smax = max(smax, pad - GVM_DEV);
            xpair(pad, r[d0]);
            budget = pn >= NPAIR ? 1 : budget;   // (the bundle that fills the pairs is the pass's last)
        }
"""
    elif not xr:
        o += "        }\n"
    o += "        d0 = wr ? 0 : d0;\n    } else "
    a = "    [branch] if (path == GVM_PATH_MEMF) {"
    assert a in src
    return src.replace(a, o + a.lstrip(), 1)

def p2strip(P, src):
    """a tick without the file or pairs: x23's text where phase 2 added branches (the state's texel 23, again_out's file,
    the general step's file ops, the commit's points)"""
    if P['xr'] or P['npair']: return src
    def rep(a, b):
        nonlocal src
        assert a in src, 'p2strip: no ' + a[:60]
        src = src.replace(a, b, 1)
    rep(" : t == 23 ? uint4(pn, 0, 0, 0) : 0;", " : 0;")
    rep("""    [branch] if (t >= T_XF && t < T_XT) return v;
    [branch] if (t >= T_XT && t < T_PB) return v & ~2u;
    [branch] if (t >= T_PB || t == 23) return 0;
""", "")
    rep("    [branch] if (mode >= GVM_M_FREAD) return file_any(w, imm, a, epcw, e, ill);", "")
    i = src.index("c2f commit_vert(uint id : SV_VertexID) {"); j = src.index("uint4 commit_frag(", i)
    src = src[:i] + """c2f commit_vert(uint id : SV_VertexID) {
    uint c = id / WCN, j = id % WCN;
    uint tg = pick(state_any(T_WT + (j >> 2), c), j & 3), tex = tg & 0x0fffffffu;
    float2 at = (float2(tex % RAM_W, tex / RAM_W) + 0.5) / float2(RAM_W, _RamRows);
    c2f o;
    o.vertex = tg != NONE && ((tg >> (28 + _Comp)) & 1) != 0 ? float4(at.x * 2 - 1, 1 - at.y * 2, 0, 1) : float4(-2, -2, 0, 1);
    o.value = pick(state_any(T_WC + j, c), _Comp);
    return o;
}
""" + src[j:]
    return src

def emit(P):
    P = full(P)
    why = valid(P)
    if why: raise ValueError(why)
    L = layout(P)
    out = [header(P, L), part('alu'), memory(P), part('taken'), fstep(P), step12(P), part('general').replace('//@@FILE_ANY@@', file_any(P)), RUN,
           state_io(P), frag(P), part('ops')]
    from memopts import memopts
    from layoutopts import layoutopts
    return layoutopts(P, memopts(P, p2strip(P, '\n'.join(out))))

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
