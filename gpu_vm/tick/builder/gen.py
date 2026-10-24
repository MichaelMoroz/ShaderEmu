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
    s12in=0,            # paths 1 and 2 (step12) inside the fast loop's loop, not after it leaves
    s12b=0,             # step12's op 0 integer or float: 0 a select of both, 1 a branch; 2 op 1's too
    cv=0,               # the commit reads one state texture a texel (a switch), an entry's value only when it is drawn
    wsh=0,              # a worker's budget a pass: core 0's (_Ticks) >> wsh
    wadf=0,             # the wad block's form: 2 before the state's load, 16 run(core == 0 ? _Ticks : bud) (18: the ALU loops as wad=0's), 1 flatten, 4 core 0's run apart
    wclean=0,           # 1: a pixel of the write cache's data texels whose entries are all unused is discarded (not written)
    c0nb=0,             # core 0's own write cache: 2 x c0nb buckets of four (0: none), its tick compiled with CORE0=1 and drawn apart
    idlew=0,            # 1: a worker parked in the firmware whose start texel is as the last pass saw it is not run: its state copied,
                        # its write cache empty (RISC-V's mc_idle; texel 23 keeps the start texel a parked pass saw). Needs npair 0
    c0rt=0,             # (c0nb) 1: core 0's cache chosen at run time in the one compile and draw (RISC-V's sizes by geometry), not CORE0=1 apart
    ll=0,               # 1: a new texel goes to the less full of its two buckets (more texels before a full end: +12-14% in a model)
    ovh=0,              # 1: the ov entries are a third table of ov / 4 hashed buckets, read when both buckets are full (slower: IDEAS 27)
    wad=0,              # 1: a worker's budget core 0's bundles of the last pass (+1/4, 512 at least) when that pass ended core 0 on sync or full
                        # (2 on full only; 3 as 2, half the budget at least; 4 as 1 unless the workers are busy; 5 on sync for
                        #  the workers outside the kernel's REST mask at WAITW, no mask no cap; 6 as 5 and on full as 2; 7 as 6, the full
                        #  cap only while the program has declared a mask: the word after the mask, WAITW + 32, not 0)
    mv=0,               # 1: a texel whose two buckets are full moves one of their residents to its other bucket, if it has room (on that path only)
    ov=0,               # overflow entries after the buckets: a store whose two buckets are full takes one (searched only then)
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
    # ---- one bundle's latency (fetch ahead): code texels asked for before they are needed
    pf=0,               # the fall-through's texel fetched an iteration ahead (fetch=both): 1 the target fetched too and a branch picks,
                        # 2 the target fetched only when taken
    pft=0,              # a touch of the code texel this many ahead each bundle (0 none): warms the texture cache for straight code
    pfd=1,              # the touch's result used 1 or 2 iterations later
    lpf=0,              # at a pass's start the core's lanes ask for this many code texels together, each lane its own, from a quarter
                        # of them behind pc on (the texture cache is cold at every pass: codemap --short --pf 1, IDEAS 40)
    lps=0,              # the same for the stack: this many RAM texels from sp up
    s12pf=0,            # step12: the fall-through, the target (and jr's register's target) fetched before the ops, picked after
    ldf=0,              # paths 3 and 5: a load whose texel the bloom word rules out takes a short branch (the texel, no lookup, no sink)
    dead=0,             # (a measure) N ops of never-run code in the fast step, behind a cbuffer bit no host sets: what the loop's size costs
    deadat=0,           # where: 0 before the path's branch, 1 at the step's end
    xtl=0,              # (a measure) N more texel loads a bundle (the next texels', folded into junk in the same step): what a load costs
    mflat=0,            # the memory paths in fewer blocks: 1 the buckets' tags read always (a select for the bloom word's skip),
                        # 2 paths 3 and 5 fetch the data texel always; 3 both
    cc0x=0,             # (ISA paths: the assembler's cc0_fast) the fast step makes j, call, jr and callr with path 7's, 3's or 5's ops: CC0X
    sil=0,              # silent stores: a store of the bytes the core sees, to a texel with no entry, makes none and needs no room
                        # (the reference's gvm_machine.silent): 1 the data texel fetched for a store whose lookup found no entry, 2 for every store,
                        # 3 only a store that finds no room looked at (fetched and compared): its texel goes on silent, the rest as before
    sil6=0,             # (sil) path 6's own: 1 its texels fetched when a store's lookup found no entry, 2 always; 0 as sil
    silp=15,            # (sil) where: 1 paths 3 and 5, 2 path 6, 4 step12's memory path, 8 the general step's stores
    silc0=0,            # (sil) 1: core 0's stores only (a `core == 0` test in each silent condition; the workers as without sil)
    snk=0,              # paths 3 and 5: a load writes no entry (the sink's tag and word) and reads no store value: a branch on the store
    m6seq=0,            # path 6 as two steps: op 0's store, then op 1 looked up in the tags op 0 left (no patched copy of its buckets)
    rmap=0,             # where texel t lies in RAM's and the ROM's textures: 0 row-major 8192 wide, 1 strips 512 x 4096 (isa.h GVM_STRIP_X; IDEAS 35),
                        # 2 pairs: texels 2k and 2k + 1 one above the other, a 32-byte sector (GVM_PAIR_X; IDEAS 40)
    cc0f=0,             # a cc 0 bundle (j, call, jr, callr) whose ops are path 7's, 3's or 5's runs the fast step's body (fstep_c), not step12
    evmax=8,            # (evf 2) compactions a pass at most
    evfx=0,             # (a measure, evf 2 and 3) 1: compact() finds nothing
    evfl=0,             # (evf 2) 1: the drop a plain loop over the entries (less code than eight fetches at once)
    evf=0,              # a store that finds both buckets full first drops their entries whose words are RAM's (each entry's texel fetched and
                        # compared; nothing a load could see changes), then takes a freed way: silent stores' entries cost room only until
                        # the cache is full. A lookup takes a matching way before a free one (a dropped entry leaves a hole before a mate)
                        # 2: the same out of the fast loop: a pass that would end full drops every entry that holds RAM's words, packs the
                        # buckets again (no hole before a resident, none in a texel's first bucket while it lies in its second), places
                        # the store that found no room and goes on (EVMAX times a pass at most); the lookups as they were.
                        # 3: as 2, run called again from frag; 4: as 3 with no packing, the lookups taking a match before a free way;
                        # 5: as 3 with no moves but the stores that found no room: a dropped entry a tombstone, freed after a bucket's last
                        # resident when no second-table texel has that bucket as its first; the lookups as they were
)

SPACE = dict(   # the values a search draws from
    nb=[16, 32, 64], bloom=[0, 1], hash=[0, 1], clear=['loop', 'unroll'],
    fetch=['both', 'one'], int_first=[0, 1], mem_order=['35', '4'], split=['branch', 'flatten'],
    block=[(8, 8), (16, 4), (4, 16), (8, 4), (4, 8), (32, 2), (16, 8), (8, 16), (32, 4), (64, 1), (32, 1), (16, 2)],
    bandx=[1, 2], per=[16, 32, 64, 128], jrow=[0, 1], perm=['id', 'bitrev', 'mul'], mul=[3, 5, 37, 77],
    early=[0, 1, 2, 3], mru=[0, 1, 2, 4], bb=[0, 1], tfirst=[0, 1], s6=[0, 1], nc=[0, 1], thin=[0, 1], thin8=[0, 1], wide3=[0, 1, 2], late=[0, 1, 2, 3], fx1=[0, 1], ov=[0, 4, 8, 16], mv=[0, 1], cv=[0, 1, 2], wsh=[0, 1, 2, 3], wad=[0, 1, 2, 3, 4, 5, 6, 7], s12b=[0, 1, 2, 3], s12in=[0, 1], rc=[0, 1, 2], dtx=['both', 'one'], rpk=['scalar', 'vec', 'wide'], pk=['scalar', 'vec', 'aos'], lay=['split', 'merged'],
    xr=[0, 64, 128, 256, 512, 1024], npair=[0, 32, 64, 128], xbind=[0, 1], xload=['pre', 'lazy'], xfast=[0, 1],
    pf=[0, 1, 2], pft=[0, 4, 8, 16], pfd=[1, 2], lpf=[0, 128, 256, 512, 1024], lps=[0, 64, 128, 256], s12pf=[0, 1], ldf=[0, 1], cc0f=[0, 1], evf=[0, 1, 2, 3, 4, 5], cc0x=[0, 1, 2, 3], m6seq=[0, 1], snk=[0, 1], rmap=[0, 1, 2], sil=[0, 1, 2, 3], silp=[1, 2, 3, 4, 8, 12, 15], mflat=[0, 1, 2, 3, 6, 7, 10],
)

def layout(P):
    nb = P['nb']
    wcn = 8 * nb + P.get('ov', 0) + 2
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
    if P['nb'] & (P['nb'] - 1) and P['hash'] != 1: return 'nb not a power of two (hash 0)'
    if P['perm'] == 'mul' and P['mul'] % 2 == 0: return 'mul even'
    if P['xr'] and (P['xr'] & (P['xr'] - 1) or P['xr'] < 4): return 'xr not a power of two'
    if P['npair'] % 2: return 'npair odd'
    if P.get('s6', 0) and not P.get('mru', 0): return 's6 without mru'
    if P.get('late', 0) and P.get('wide3', 0): return 'late with wide3'
    if P.get('vec', 0) and P.get('rpk', 'scalar') != 'wide': return 'vec needs rpk=wide'
    if P.get('wide3', 0) and (P.get('thin', 0) or P.get('thin8', 0)): return 'wide3 with thin'
    if P.get('thin8', 0) and (P['fetch'] != 'both' or P.get('mru', 0)): return 'thin8 with fetch=both only'
    if P['xr'] > 1024 or P['npair'] > 256: return 'file or pairs too large'
    if P.get('mv', 0) and (P.get('ov', 0) or P.get('mru', 0) or P.get('bb', 0)): return 'mv with ov, mru or bb'
    if (P.get('pf', 0) or P.get('pft', 0)) and (P['fetch'] != 'both' or P.get('thin8', 0)): return 'pf, pft with fetch=both only'
    if P.get('cc0f', 0) and (P['fetch'] != 'both' or P['xr'] or P['npair'] or P.get('mru', 0) or P.get('bb', 0) or P.get('thin', 0) or P.get('thin8', 0)
                             or P.get('wide3', 0) or P.get('late', 0) or P.get('vec', 0) or P.get('s12in', 0) or P.get('mv', 0)): return 'cc0f with these'
    if P.get('cc0x', 0) and (P['fetch'] != 'both' or P.get('late', 0) or P.get('thin8', 0) or P.get('pf', 0) or P.get('wide3', 0)): return 'cc0x with these'
    if P.get('snk', 0) and (P.get('mru', 0) or P.get('bb', 0) or P['xr'] or P['npair']): return 'snk with mru, bb or the file'
    if P.get('m6seq', 0) and (P.get('mru', 0) or P.get('bb', 0) or P.get('mv', 0) or P.get('mflat', 0) & 4): return 'm6seq with mru, bb, mv or mflat 4'
    if P.get('ldf', 0) and (not P['bloom'] or P['xr'] or P['npair'] or P.get('nc', 0) or P.get('mru', 0) or P.get('bb', 0) or P.get('rc', 0)): return 'ldf with bloom only'
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
static const uint MV = {P.get('mv', 0)};   // (mv: the move on a full pair of buckets)
static bool mvok = false;   // (mv: on for a store's lookup whose store fills the slot it frees: a free way must not hide its bucket's texel in the other)
{"static bool evok = false;   // (evf: on for a store's lookup: a full pair of buckets drops its entries that hold RAM's words)" + chr(10) if P.get('evf', 0) == 1 else ''}static const uint OV = {P.get('ov', 0)}, OVB = 8 * NB;   // overflow entries from OVB, searched when both buckets are full
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
static const uint WSH = {P.get('wsh', 0)};
static const uint WAD = {P.get('wad', 0)};{sil_decl(P)}
static const uint WAITW = 0x00c01100;   // (WAD 5) the kernel's REST mask: 8 words, bit k = worker k (core 0 waits for its job), one pass after a rest   // a worker's budget: core 0's >> WSH; WAD: core 0's last pass's when it ended on sync or full
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
static uint thr, bud;   // (bud: this core's budget this pass)
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
    elif P['nb'] & (P['nb'] - 1):   # (any count: the high part of a 16-bit fraction, as core0's hashes)
        h = '''uint wc_b1(uint t) { return (((t * 0x9e3779b1u) >> 16) * NB) >> 16; }
uint wc_b2(uint t) { return NB + ((((t * 0x85ebca6bu + 0x165667b1u) >> 16) * NB) >> 16); }'''
    else:
        h = f'''uint wc_b1(uint t) {{ return (t * 0x9e3779b1u) >> {32 - lg}; }}
uint wc_b2(uint t) {{ return NB + ((t * 0x85ebca6bu + 0x165667b1u) >> {32 - lg}); }}'''
    if P.get('ovh', 0):
        lo = int(math.log2(max(P['ov'] // 4, 1)))
        h += f'''
uint wc_b3(uint t) {{ return (t * 0xc2b2ae35u + 0x27d4eb2fu) >> {32 - lo}; }}'''
    look = "st || ((ad >> 4) & bloom) == (ad >> 4)" if P['bloom'] else 'true'
    evf = P.get('evf', 0) == 1
    evk = P.get('evf', 0) in (1, 4)   # (a match before a free way: holes allowed)
    ll = ("""    uint ka_ = firstbitlow(msk | 16u), kb_ = firstbitlow((msk >> 4) | 16u);   // (ll) a bucket's ways fill in order: the first
    bool am = ka_ < 4 && pick(m.A, ka_ & 3) != NONE, bm = kb_ < 4 && pick(m.B, kb_ & 3) != NONE;   // match or free is its fill
    uint k = am ? ka_ : bm ? 4 + kb_ : kb_ < ka_ ? 4 + kb_ : ka_ < 4 ? ka_ : 8u;   // a new texel to the less full bucket
""" if P.get('ll', 0) else EVFK if evk else "    uint k = firstbitlow(msk);" + chr(10))
    ovh = "    [branch] if (OV != 0 && k >= 8) {   // (ovh) both buckets full: the texel's bucket of a third table\n        uint b3 = (OVB >> 2) + wc_b3(m.t);\n        uint4 C = bucket(b3);\n        bool4 kc = (C & 0x0fffffffu) == m.t || C == NONE;\n        uint j = kc.x ? 0u : kc.y ? 1u : kc.z ? 2u : kc.w ? 3u : 4u;\n        m.otag = j < 4 ? pick(C, j) : NONE; m.sl = j < 4 ? 4 * b3 + j : NONE;\n    } else" if P.get('ovh', 0) else ''
    return ('''// ---- Memory: the write cache holds a texel in an entry of its bucket in the first table or the second
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
@LL@    m.otag = k < 8 ? pick(k < 4 ? m.A : m.B, k & 3) : NONE;
    m.sl = k < 4 ? 4 * m.b1 + k : k < 8 ? 4 * m.b2 + (k & 3) : NONE;
@OVH@    [branch] if (OV != 0 && k >= 8) {   // both buckets full: the texel's overflow entry, or the first free one
        uint j = OV, q;
        [loop] for (q = 0; q < OV; q++) { uint g = wt[OVB + q]; j = j == OV && ((g & 0x0fffffffu) == m.t || g == NONE) ? q : j; }
        m.otag = j < OV ? wt[OVB + j] : NONE; m.sl = j < OV ? OVB + j : NONE;
    }
    [branch] if (MV != 0 && OV == 0 && k >= 8 && mvok) {   // (mv) both full: a resident with room in its other bucket moves there, its slot is the texel's
        uint j, f = NONE, from = NONE;
        [loop] for (j = 0; j < 8 && f == NONE; j++) {
            uint u = pick(j < 4 ? m.A : m.B, j & 3) & 0x0fffffffu, ob = j < 4 ? wc_b2(u) : wc_b1(u);
            uint4 o = bucket(ob);
            uint w = o.x == NONE ? 0u : o.y == NONE ? 1u : o.z == NONE ? 2u : o.w == NONE ? 3u : 4u;
            from = w < 4 ? (j < 4 ? 4 * m.b1 + j : 4 * m.b2 + (j & 3)) : from; f = w < 4 ? 4 * ob + w : f;
        }
        [branch] if (f != NONE) {
            uint l = from | 3;   // (a bucket keeps no hole, a lookup takes its first free way: its last resident fills the moved one's way)
            wt[f] = wt[from]; wd[4 * f] = wd[4 * from]; wd[4 * f + 1] = wd[4 * from + 1]; wd[4 * f + 2] = wd[4 * from + 2]; wd[4 * f + 3] = wd[4 * from + 3];
            wt[from] = wt[l]; wd[4 * from] = wd[4 * l]; wd[4 * from + 1] = wd[4 * l + 1]; wd[4 * from + 2] = wd[4 * l + 2]; wd[4 * from + 3] = wd[4 * l + 3];
            wt[l] = NONE; m.sl = l; m.otag = NONE;
            m.A = bucket(m.b1); m.B = bucket(m.b2);
        }
    }
@EVF@    m.e = has(m.otag, m.t, m.i) ? m.sl : WCN;
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
''').replace('@OVH@', ovh).replace('@LL@', ll).replace('@EVF@', EVF if evf else '')

EVFK = '''    uint mt = (ka.x && m.A.x != NONE ? 1u : 0u) | (ka.y && m.A.y != NONE ? 2u : 0u) | (ka.z && m.A.z != NONE ? 4u : 0u) | (ka.w && m.A.w != NONE ? 8u : 0u)
             | (kb.x && m.B.x != NONE ? 16u : 0u) | (kb.y && m.B.y != NONE ? 32u : 0u) | (kb.z && m.B.z != NONE ? 64u : 0u) | (kb.w && m.B.w != NONE ? 128u : 0u);
    uint k = firstbitlow(mt != 0 ? mt : msk);   // (evf: a match before a free way: a dropped entry leaves holes)
'''
EVF = '''    [branch] if (k >= 8 && evok) {   // (evf) both buckets full for a store: drop the entries whose words are RAM's
        uint j, fr = 0;
        [unroll] for (j = 0; j < 8; j++) {
            uint g = pick(j < 4 ? m.A : m.B, j & 3), e = j < 4 ? 4 * m.b1 + j : 4 * m.b2 + (j & 3);
            uint4 v = ram(g & 0x0fffffffu);
            uint mk = g >> 28;
            bool eq = ((mk & 1) == 0 || wd[4 * e] == v.x) && ((mk & 2) == 0 || wd[4 * e + 1] == v.y)
                   && ((mk & 4) == 0 || wd[4 * e + 2] == v.z) && ((mk & 8) == 0 || wd[4 * e + 3] == v.w);
            fr |= eq ? 1u << j : 0u;
        }
        [branch] if (fr != 0) {
            [loop] for (j = 0; j < 8; j++) [branch] if ((fr >> j) & 1) wt[j < 4 ? 4 * m.b1 + j : 4 * m.b2 + (j & 3)] = NONE;
            m.A = bucket(m.b1); m.B = bucket(m.b2);
            k = firstbitlow(fr);
            m.otag = NONE; m.sl = k < 4 ? 4 * m.b1 + k : 4 * m.b2 + (k & 3);
        }
    }
'''
MEM35 = '''[branch] if ((path & 1) != 0) {   // a load or a store, and a lite op (paths 3 and 5)
            bool st0 = (t.x & op_bit(8)) != 0;
            mvok = MV != 0 && st0; mop m0 = mread_entries(t.x, i0, A0, B0, st0); mvok = false;
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
            mvok = MV != 0 && st1; mfind(m1); mvok = false;
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
MEM6SEQ = '''{   // a store, then a load or a store looked up after it (path 6 as two steps: m6seq)
            bool st1 = (t.y & op_bit(8)) != 0;
            mop m0 = mread_entries(t.x, i0, A0, B0, true);
            [branch] if (((t.x & op_bit(3)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);
            uint w0 = seen(m0), sl0 = slot(m0), s0x = sl0 == NONE ? WCS : sl0;
            uint tg0 = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wt[s0x] = tg0; wd[4 * s0x + m0.i] = merge(t.x, w0, m0.ad, r[d0]);
            bloom |= m0.t;
            mop m1 = mread_entries(t.y, i1, A1, B1, true);
            bool over = m0.t == m1.t && sl0 == NONE;   // (op 0 took the spare slot: op 1 of its texel sees it there)
            m1.sl = over ? WCS : m1.sl; m1.otag = over ? tg0 : m1.otag; m1.e = over ? (has(tg0, m1.t, m1.i) ? WCS : WCN) : m1.e;
            m1.w = wd[4 * m1.e + m1.i];
            [branch] if ((((t.y ^ op_bit(8)) & op_bit(11)) | (m1.ad & 3)) != 0) m1.q = data_texel(m1.ad);
            smax = max(smax, max(m0.ad - GVM_DEV, st1 ? m1.ad - GVM_DEV : 0)); lmax = max(lmax, st1 ? 0 : m1.ad & ~GVM_ROM);
            uint w1 = seen(m1), sl1 = m1.sl;
            v1 = extend(t.y, w1, m1.ad);
            bool nr = sl0 == NONE || (st1 && sl1 == NONE);
            uint at1 = st1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN;
            wt[at1] = stag(m1.otag, m1.t, m1.i);
            wd[4 * at1 + m1.i] = merge(t.y, w1, m1.ad, r[d1]);
            bloom |= st1 ? m1.t : 0;
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

LDF = '''uint lad = mem_addr(t.x, i0, A0, B0);   // (ldf)
        [branch] if ((path & 1) != 0 && (t.x & op_bit(8)) == 0 && ((lad >> 4) & bloom) != (lad >> 4)) {   // (ldf: a load whose texel no store of the pass can hold)
            lmax = max(lmax, lad & ~GVM_ROM);
            v0 = extend(t.x, pick(data_texel(lad), (lad >> 2) & 3), lad);
            r[s0] = v0;
            v1 = alu_lite(t.y, i1, ra1 == s0 ? v0 : A1, rb1 == s0 ? v0 : B1);
        } else '''

def mflat(P, src):
    '''mflat: the memory paths' loads in one block with the rest (no branch before them)'''
    f = P.get('mflat', 0)
    if f & 1:
        a = '''    [branch] if (look) {
        m.A = bucket(m.b1); m.B = bucket(m.b2);
    }
'''
        assert src.count(a) == 1
        src = src.replace(a, '''    uint4 ta = bucket(m.b1), tb = bucket(m.b2);   // (mflat 1)
    m.A = look ? ta : NONE; m.B = look ? tb : NONE;
''')
    if f & 2:
        a = "            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);\n"
        assert src.count(a) == 1
        src = src.replace(a, "            m0.q = data_texel(m0.ad);   // (mflat 2)\n")
    if f & 8:   # (paths 3 and 5 alone read the tags always: mlook_f; path 4 keeps its branch)
        i = src.index('mop mlook(uint ad, bool look) {'); j = src.index('\n}\n', i) + 3
        k = src.index('mop mread_entries(uint w, uint imm, uint a, uint b, bool st) {'); l = src.index('\n}\n', k) + 3
        fl = src[i:j].replace('mop mlook(', 'mop mlook_f(').replace('''    [branch] if (look) {
        m.A = bucket(m.b1); m.B = bucket(m.b2);
    }
''', '''    uint4 ta = bucket(m.b1), tb = bucket(m.b2);   // (mflat 8)
    m.A = look ? ta : NONE; m.B = look ? tb : NONE;
''')
        assert 'mflat 8' in fl
        fe = src[k:l].replace('mop mread_entries(', 'mop mread_entries_f(').replace('return mlook(', 'return mlook_f(')
        src = src[:l] + fl + fe + src[l:]
        a = "mop m0 = mread_entries(t.x, i0, A0, B0, st0);"
        assert src.count(a) == 1
        src = src.replace(a, "mop m0 = mread_entries_f(t.x, i0, A0, B0, st0);")
    if f & 4:
        a = "            [branch] if (((t.x & op_bit(3)) | ((t.y ^ op_bit(8)) & op_bit(11)) | ((m0.ad | m1.ad) & 3)) != 0) { m0.q = data_texel(m0.ad); m1.q = data_texel(m1.ad); }\n"
        assert src.count(a) == 1
        src = src.replace(a, "            m0.q = data_texel(m0.ad); m1.q = data_texel(m1.ad);   // (mflat 4)\n")
    return src

def sil_decl(P):
    return ('\nstatic const uint SIL = %d;   // (sil) a store of what the core sees, to a texel with no entry, makes none (gvm_machine.silent)' % P['sil']) if P.get('sil', 0) else ''

def rmap_text(P, src):
    """rmap 1: RAM's and the ROM's texel t at (GVM_STRIP_X(t), GVM_STRIP_Y(t)) (isa.h): ram(), data_texel(), the commit's points,
    the operations' writes (a piece lies in one 4 KB block, so in one strip row: one quad) and their texel from a pixel"""
    if not P.get('rmap', 0): return src
    def rep(a, b, n=1):
        nonlocal src
        assert src.count(a) >= n, 'rmap: ' + a[:70]
        src = src.replace(a, b)
    if P['rmap'] == 2:   # (pairs: a piece of the operations, in one 4 KB block, is one box 128 x 2; the frag drops texels outside it)
        rep("static const uint RAM_W = 8192;", "static const uint RAM_W = 8192;\nstatic const uint RAM_MAP = 2;   // (rmap) texel t at (GVM_PAIR_X(t), GVM_PAIR_Y(t)): pairs, one above the other\n"
            "int3 rat(uint t) { return int3(GVM_PAIR_X(t), GVM_PAIR_Y(t), 0); }")
        rep("return _Ram.Load(int3(t % RAM_W, t / RAM_W, 0));", "return _Ram.Load(rat(t));")
        rep("int3 at = int3(t % RAM_W, (t / RAM_W) & 0x1fff, 0);", "int3 at = rat(t & 0x3ffffffu);")
        rep("float2 at = (float2(tex % RAM_W, tex / RAM_W) + 0.5) / float2(RAM_W, _RamRows);",
            "float2 at = (float2(GVM_PAIR_X(tex), GVM_PAIR_Y(tex)) + 0.5) / float2(RAM_W, _RamRows);")
        rep("x0 = t0 % RAM_W, y0 = t0 / RAM_W, xe = x0 + t1 - t0 + 1;", "x0 = GVM_PAIR_X(t0), y0 = GVM_PAIR_Y(t0) & ~1u, xe = GVM_PAIR_X(t1) + 1;")
        rep("""    r.vertex = corner(vid, second ? 0 : (float)x0, (float)(second ? xe - RAM_W : min(xe, RAM_W)), (float)(y0 + (second ? 1 : 0)),
                      (float)(y0 + (second ? 2 : 1)), (float)RAM_W, (float)_RamRows, on && (!second || xe > RAM_W));""",
            """    r.vertex = corner(vid, (float)x0, (float)xe, (float)y0, (float)(y0 + 2), (float)RAM_W, (float)_RamRows, on && !second);   // (rmap 2: one box)""")
        rep("uint t = px.y * RAM_W + px.x;", "uint t = GVM_PAIR_T(px.x, px.y);")
        assert '% RAM_W' not in src and 't / RAM_W' not in src, 'rmap: a row-major address left'
        return src
    rep("static const uint RAM_W = 8192;", "static const uint RAM_W = 8192;\nstatic const uint RAM_MAP = 1;   // (rmap) texel t at (GVM_STRIP_X(t), GVM_STRIP_Y(t)): strips 512 x 4096\n"
        "int3 rat(uint t) { return int3(GVM_STRIP_X(t), GVM_STRIP_Y(t), 0); }")
    rep("return _Ram.Load(int3(t % RAM_W, t / RAM_W, 0));", "return _Ram.Load(rat(t));")
    rep("int3 at = int3(t % RAM_W, (t / RAM_W) & 0x1fff, 0);", "int3 at = rat(t & 0x3ffffffu);")
    rep("float2 at = (float2(tex % RAM_W, tex / RAM_W) + 0.5) / float2(RAM_W, _RamRows);",
        "float2 at = (float2(GVM_STRIP_X(tex), GVM_STRIP_Y(tex)) + 0.5) / float2(RAM_W, _RamRows);")
    rep("x0 = t0 % RAM_W, y0 = t0 / RAM_W, xe = x0 + t1 - t0 + 1;", "x0 = GVM_STRIP_X(t0), y0 = GVM_STRIP_Y(t0), xe = x0 + t1 - t0 + 1;")
    rep("uint t = px.y * RAM_W + px.x;", "uint t = GVM_STRIP_T(px.x, px.y);")
    assert '% RAM_W' not in src and 't / RAM_W' not in src, 'rmap: a row-major address left'
    return src

def sil_text(P, src):
    """sil: a store whose bytes are what the core sees already, to a texel with no entry this pass, makes no entry and needs no
    room (no full end); a store to a texel with an entry updates it as before. 1: the data texel is fetched for a store whose
    lookup found no entry, 2: for every store (paths 3, 5 and 6; step12 and the general step fetch it always)"""
    k, wh = P.get('sil', 0), P.get('silp', 15)
    k6 = P.get('sil6', 0) or k   # (path 6's fetch: sil6 1 or 2, else sil's)
    if not k: return src
    def rep(a, b, n=1):
        nonlocal src
        assert src.count(a) == n, 'sil: ' + a[:70]
        src = src.replace(a, b)
    # paths 3 and 5
    if wh & 1: rep("            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);\n",
        "            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3) | (st0 && %s ? 1u : 0u)) != 0) m0.q = data_texel(m0.ad);   // (sil %d)\n"
        % ('m0.otag == NONE' if k != 2 else 'true', k))
    if wh & 1: rep("            bool nr = st0 && sl0 == NONE;\n            uint at0 = st0 ? (nr ? WCS : sl0) : WCN;\n",
        "            bool sil0 = st0 && m0.otag == NONE && merge(t.x, w0, m0.ad, r[d0]) == w0;   // (sil)\n"
        "            bool nr = st0 && sl0 == NONE && !sil0;\n            uint at0 = st0 && !sil0 ? (nr ? WCS : sl0) : WCN;\n")
    # path 6
    if wh & 2: rep("            [branch] if (((t.x & op_bit(3)) | ((t.y ^ op_bit(8)) & op_bit(11)) | ((m0.ad | m1.ad) & 3)) != 0) { m0.q = data_texel(m0.ad); m1.q = data_texel(m1.ad); }\n",
        "            [branch] if (((t.x & op_bit(3)) | ((t.y ^ op_bit(8)) & op_bit(11)) | ((m0.ad | m1.ad) & 3) | (%s ? 1u : 0u)) != 0) { m0.q = data_texel(m0.ad); m1.q = data_texel(m1.ad); }   // (sil %d)\n"
        % ('m0.otag == NONE || (st1 && m1.otag == NONE)' if k6 != 2 else 'true', k6))
    if wh & 2: rep("            bool same = m0.t == m1.t, over = same && sl0 == NONE;\n            wt[s0x] = tg0;",
        "            bool sil0 = m0.otag == NONE && e0 == w0;   // (sil)\n            s0x = sil0 ? WCN : s0x;\n"
        "            bool same = m0.t == m1.t, over = same && sl0 == NONE && !sil0;\n            wt[s0x] = tg0;")
    if wh & 2: rep("""            v1 = extend(t.y, w1, m1.ad);
            bool nr = sl0 == NONE || (st1 && sl1 == NONE);
            uint o1 = m1.otag;
            uint at1 = st1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN;
""", """            v1 = extend(t.y, w1, m1.ad);
            bool sil1 = st1 && m1.otag == NONE && merge(t.y, w1, m1.ad, r[d1]) == w1;   // (sil)
            bool nr = (sl0 == NONE && !sil0) || (st1 && sl1 == NONE && !sil1);
            uint o1 = m1.otag;
            uint at1 = st1 && !sil1 ? (sl1 == NONE ? WCS + 1 : sl1) : WCN;
""")
    # step12's memory path (mread fetches the texel)
    if wh & 4: rep("""        ok = !stv || sl != NONE;
        uint at = stv && ok ? sl : WCN;
""", """        bool sil = stv && m.otag == NONE && merge(t.x, word, m.ad, r[d0]) == word;   // (sil)
        ok = !stv || sl != NONE || sil;
        uint at = stv && ok && !sil ? sl : WCN;
""")
    # the general step's stores, a word at a time: a word of what is there, to a texel with no entry, goes to the sink
    if wh & 8: rep("""        uint s = lk.sl;
        ok = s != NONE;
        [branch] if (ok) {
            uint tg = lk.otag;
            uint e = wd[4 * s + i];
            uint old = lk.e != WCN ? lk.w : pick(ram(t), i);
            uint lo = k == 0 ? o : 0, hi = min(4u, o + by - 4 * k);
            uint m = (hi >= 4 ? 0xffffffffu : (1u << (8 * hi)) - 1) & ~((1u << (8 * lo)) - 1);
            uint val = o != 0 ? (word_of(d, k) << (8 * o)) | (k != 0 ? word_of(d, k - 1) >> (32 - 8 * o) : 0) : word_of(d, k);
""", """        uint s = lk.sl;
        uint old = lk.e != WCN ? lk.w : pick(ram(t), i);   // (sil: before the room's test)
        uint lo = k == 0 ? o : 0, hi = min(4u, o + by - 4 * k);
        uint m = (hi >= 4 ? 0xffffffffu : (1u << (8 * hi)) - 1) & ~((1u << (8 * lo)) - 1);
        uint val = o != 0 ? (word_of(d, k) << (8 * o)) | (k != 0 ? word_of(d, k - 1) >> (32 - 8 * o) : 0) : word_of(d, k);
        s = lk.otag == NONE && ((old ^ val) & m) == 0 ? WCN : s;   // (sil: no entry, no room needed: the sink)
        ok = s != NONE;
        [branch] if (ok) {
            uint tg = lk.otag;
            uint e = wd[4 * s + i];
""")
    if k == 3:   # (3: only a store that finds no room is looked at: its texel fetched, compared, and the pass goes on if it is silent)
        src = '\n'.join(l.replace('m0.otag == NONE', 'm0.sl == NONE').replace('m1.otag == NONE', 'm1.sl == NONE').replace('m.otag == NONE', 'm.sl == NONE')
                        .replace('lk.otag == NONE', 'lk.sl == NONE') if '(sil' in l else l for l in src.split('\n'))
    if P.get('silc0', 0):   # (core 0 alone: every silent condition behind core == 0, the workers' stores as without sil)
        import re
        assert k != 2 and P.get('sil6', 0) != 2, 'silc0: sil 1 or 3'
        src = '\n'.join(re.sub(r'\b((?:m0|m1|m|lk)\.(?:otag|sl) == NONE)', r'(core == 0 && \1)', l) if '(sil' in l else l
                        for l in src.split('\n'))
    return src

def evf_text(P, src):
    '''evf: evok on for the stores' lookups (paths 3, 5 and 6, step12, the general step's stores)'''
    if not P.get('evf', 0): return src
    if P['evf'] >= 2: return evf2_text(P, src)
    assert not (P.get('mru', 0) or P.get('bb', 0) or P.get('mv', 0) or P.get('ov', 0) or P.get('ll', 0) or P.get('m6seq', 0) or P.get('ovh', 0)), 'evf: a plain cache'
    def rep(a, b, n=1):
        nonlocal src
        assert src.count(a) == n, 'evf: ' + a[:70]
        src = src.replace(a, b)
    rep("mvok = MV != 0 && st0; mop m0 = mread_entries(t.x, i0, A0, B0, st0); mvok = false;",
        "mvok = MV != 0 && st0; evok = st0; mop m0 = mread_entries(t.x, i0, A0, B0, st0); mvok = false; evok = false;")
    rep("mop m0 = mread_entries(t.x, i0, A0, B0, true), m1 = mread_entries(t.y, i1, A1, B1, true);",
        "evok = true; mop m0 = mread_entries(t.x, i0, A0, B0, true); evok = false; mop m1 = mread_entries(t.y, i1, A1, B1, true);   // (evf: op 0's lookup may drop entries before op 1's buckets are read)")
    rep("mvok = MV != 0 && st1; mfind(m1); mvok = false;", "mvok = MV != 0 && st1; evok = st1; mfind(m1); mvok = false; evok = false;")
    rep("        mop m = mread(t.x, i0, A0, B0);", "        evok = (t.x & op_bit(8)) != 0; mop m = mread(t.x, i0, A0, B0); evok = false;")
    rep("        mop lk = mlook(wi << 2, true);", "        evok = true; mop lk = mlook(wi << 2, true); evok = false;")
    return src

EVF2 = """static uint evn;   // (evf 2: this pass's compactions)
void ev_move(uint f, uint to) {   // (evf 2) entry f to the free entry to
    wt[to] = wt[f]; wd[4 * to] = wd[4 * f]; wd[4 * to + 1] = wd[4 * f + 1]; wd[4 * to + 2] = wd[4 * f + 2]; wd[4 * to + 3] = wd[4 * f + 3];
    wt[f] = NONE;
}
void ev_pack(uint b0, uint b1) {   // (evf 2) buckets b0 to b1 - 1: their residents to the front, in order
    uint b, w;
    [loop] for (b = b0; b < b1; b++) {
        uint k = 4 * b;
        [loop] for (w = 0; w < 4; w++) [branch] if (wt[4 * b + w] != NONE) { [branch] if (k != 4 * b + w) ev_move(4 * b + w, k); k++; }
    }
}
uint ev_free(uint b) { uint4 q = bucket(b); return q.x == NONE ? 4 * b : q.y == NONE ? 4 * b + 1 : q.z == NONE ? 4 * b + 2 : q.w == NONE ? 4 * b + 3 : NONE; }
bool compact() {   // (evf 2) a full write cache: drop the entries that hold RAM's words, pack, place the stores that found no room
    uint g, j, n = 0;
    [loop] for (g = 0; g < 8 * NB; g += 8) {
        uint4 v[8];
        [unroll] for (j = 0; j < 8; j++) { uint tg = wt[g + j]; v[j] = ram(tg & 0x0fffffffu); }
        [unroll] for (j = 0; j < 8; j++) {
            uint e = g + j, tg = wt[e], mk = tg >> 28;
            bool eq = tg != NONE && ((mk & 1) == 0 || wd[4 * e] == v[j].x) && ((mk & 2) == 0 || wd[4 * e + 1] == v[j].y)
                   && ((mk & 4) == 0 || wd[4 * e + 2] == v[j].z) && ((mk & 8) == 0 || wd[4 * e + 3] == v[j].w);
            [flatten] if (eq) wt[e] = NONE;
            n += eq ? 1u : 0u;
        }
    }
    [branch] if (n == 0) return false;
    ev_pack(0, 2 * NB);
    [loop] for (g = 4 * NB; g < 8 * NB; g++) {   // a second bucket's resident whose first bucket has room moves there
        uint tg = wt[g];
        [branch] if (tg != NONE) { uint f = ev_free(wc_b1(tg & 0x0fffffffu)); [branch] if (f != NONE) ev_move(g, f); }
    }
    ev_pack(NB, 2 * NB);
    bool ok = true;
    [loop] for (g = WCS; g < WCS + 2; g++) {   // the stores that found no room: their texel's first bucket, else its second
        uint tg = wt[g];
        [branch] if (tg != NONE) {
            uint t = tg & 0x0fffffffu, f = ev_free(wc_b1(t));
            f = f != NONE ? f : ev_free(wc_b2(t));
            [branch] if (f != NONE) ev_move(g, f); else ok = false;
        }
    }
    return ok;
}
"""

def evf2_text(P, src):
    """evf 2: compact() before run(), and run's loop goes on after a full end that it frees"""
    assert not (P.get('mru', 0) or P.get('bb', 0) or P.get('mv', 0) or P.get('ov', 0) or P.get('ll', 0) or P.get('ovh', 0) or P.get('rmap', 0)), 'evf 2: a plain cache'
    def rep(a, b):
        nonlocal src
        assert src.count(a) == 1, 'evf 2: ' + a[:70]
        src = src.replace(a, b)
    N = chr(10)
    ev = EVF2
    if P.get('evfx', 0) == 1:   # (a measure: compact() finds nothing; the structure's own cost)
        ev = ev[:ev.index('bool compact() {')] + 'bool compact() { return false; }' + chr(10)
    if P.get('evfx', 0) == 2:   # (a measure: the drop alone, then the pass ends as before)
        ev = ev.replace('    [branch] if (n == 0) return false;', '    return false;')
        ev = ev[:ev.index('    ev_pack(0, 2 * NB);')] + '}' + chr(10)
    if P.get('evfx', 0) == 3:   # (a measure: the drop and the packing, then the pass ends as before)
        ev = ev.replace('    ev_pack(NB, 2 * NB);', '    ev_pack(NB, 2 * NB);' + chr(10) + '    return false;')
    if P.get('evfl', 0):   # (evfl 1: the drop a plain loop, one entry at a time: less code)
        i = ev.index('    [loop] for (g = 0; g < 8 * NB; g += 8) {'); j = ev.index('    [branch] if (n == 0) return false;')
        ev = ev[:i] + '''    [loop] for (g = 0; g < 8 * NB; g++) {
        uint tg = wt[g], mk = tg >> 28;
        uint4 v = ram(tg & 0x0fffffffu);
        bool eq = tg != NONE && ((mk & 1) == 0 || wd[4 * g] == v.x) && ((mk & 2) == 0 || wd[4 * g + 1] == v.y)
               && ((mk & 4) == 0 || wd[4 * g + 2] == v.z) && ((mk & 8) == 0 || wd[4 * g + 3] == v.w);
        [flatten] if (eq) wt[g] = NONE;
        n += eq ? 1u : 0u;
    }
''' + ev[j:]
    rep("void run(uint budget) {", ev + "void run(uint budget) {")
    if P['evf'] == 4:   # (evf 4: no packing: the lookups take a match before a free way, so a hole hides nothing)
        a = ev[ev.index('    ev_pack(0, 2 * NB);'):ev.index('    bool ok = true;')]
        assert src.count(a) == 1
        src = src.replace(a, '')
    if P['evf'] == 5:   # (evf 5: a dropped entry's tag a tombstone (no texel, no word: a lookup passes it, the commit draws nothing);
        # those after a bucket's last resident become free when no texel of the second table has the bucket as its first
        rep("            bool eq = tg != NONE && ((mk & 1)", "            bool eq = tg != NONE && tg != TOMB && ((mk & 1)")
        a = ev[ev.index('            [flatten] if (eq) wt[e] = NONE;'):ev.index('    bool ok = true;')]
        assert src.count(a) == 1
        src = src.replace(a, '''            [flatten] if (eq) wt[e] = TOMB;
            n += eq ? 1u : 0u;
        }
    }
    [branch] if (n == 0) return false;
    uint dep0 = 0, dep1 = 0;   // (the first-table buckets a second-table resident has as its first: they must stay full)
    [loop] for (g = 4 * NB; g < 8 * NB; g++) {
        uint tg = wt[g];
        [branch] if (tg != NONE && tg != TOMB) { uint b = wc_b1(tg & 0x0fffffffu); dep0 |= b < 32 ? 1u << b : 0u; dep1 |= b >= 32 ? 1u << (b - 32) : 0u; }
    }
    [loop] for (g = 0; g < 2 * NB; g++) {   // a bucket's tombstones after its last resident: free
        uint4 q = bucket(g);
        bool keep = g < NB && (((g < 32 ? dep0 >> g : dep1 >> (g - 32)) & 1) != 0);
        bool l3 = q.w != NONE && q.w != TOMB, l2 = l3 || (q.z != NONE && q.z != TOMB), l1 = l2 || (q.y != NONE && q.y != TOMB);
        [branch] if (!keep) {
            [flatten] if (!l1 && q.y == TOMB) wt[4 * g + 1] = NONE;
            [flatten] if (!l2 && q.z == TOMB) wt[4 * g + 2] = NONE;
            [flatten] if (!l3 && q.w == TOMB) wt[4 * g + 3] = NONE;
            [flatten] if (!l1 && !(q.x != NONE && q.x != TOMB) && q.x == TOMB) wt[4 * g] = NONE;
        }
    }
''')
        rep("static const uint NONE = 0xffffffffu;", "static const uint NONE = 0xffffffffu;" + N + "static const uint TOMB = 0x0ffffffeu;   // (evf 5: a dropped entry: no texel's, no word)")
    if P['evf'] in (3, 4, 5):   # (evf 3: run as it was, called again from frag after a full end that compact() frees)
        calls = ["    run(core == 0 ? _Ticks : bud);" + N, "    run(bud);" + N]
        c = [x for x in calls if src.count(x) == 1]
        assert len(c) == 1, 'evf 3: run call'
        rep("    [branch] if (!ok) return 0;" + N, "    [branch] if (!ok) { wfull = true; return 0; }   // (evf 3: no room)" + N)
        rep("uint step12(uint budget) {", "static bool wfull = false;   // (evf 3: step12's store found no room)" + N + "uint step12(uint budget) {")
        arg = c[0].strip()[4:-2]
        rep(c[0], N.join([
            "    uint rb = " + arg + ", c0 = cycle;   // (evf 3) a full end that compact() frees: run again with what is left of the budget",
            "    [loop] for (evn = 0; evn <= %du; evn++) {   // (one call of run: its text once)" % P.get('evmax', 8),
            "        run(rb - (cycle - c0));",
            "        [branch] if (evn == %du || !(endr == E_FULL || (endr == E_BUDGET && (wfull || wt[WCS] != NONE || wt[WCS + 1] != NONE)))) break;   // (HLSL makes both sides of || )" % P.get('evmax', 8),
            "        [branch] if (!compact()) break;",
            "        wfull = false;",
            "        endr = E_BUDGET;",
            "    }", ""]))
        return src
    a = "    uint guard = budget + 8;   // a hard cap on the trips of both loops, whatever the budget does" + N
    rep(a, a + "    uint rb = budget, c0 = cycle;   // (evf 2)" + N + "    evn = 0;" + N)
    a = "        else [branch] if (budget > 0) budget = general(budget);" + N + "    }" + N
    rep(a, a[:-6] + N.join([
        "        [branch] if (budget == 0 && cycle - c0 < rb && (endr == E_BUDGET || endr == E_FULL) && evn < %du) {   // (evf 2) a full end: room made, the pass goes on" % P.get('evmax', 8),
        "            evn++;",
        "            [branch] if (compact()) { endr = E_BUDGET; budget = rb - (cycle - c0); guard = budget + 8; }",
        "        }", "    }", ""]))
    return src

def cc0x_text(P, src):
    '''cc0x: j, call, jr and callr in the fast step (the assembler gives them paths 7, 3, 5 when their ops fit and none writes jr's rs)'''
    if not P.get('cc0x', 0): return src
    def rep(a, b):
        nonlocal src
        assert src.count(a) == 1, 'cc0x: ' + a[:60]
        src = src.replace(a, b)
    rep("    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18;\n",
        "    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18;\n"
        "    uint sb = (ctl >> 11) & 7;\n    bool c0 = (ctl & 0x780) == 0, ind = c0 && (sb == GVM_SUB_JR || sb == GVM_SUB_CALLR), cl = c0 && (sb == GVM_SUB_CALL || sb == GVM_SUB_CALLR);   // (cc0x)\n")
    if P['cc0x'] == 1:
        rep("    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;\n",
            "    uint tg = c0 ? (ind ? r[(ctl >> 14) & 63] >> 4 : tp + asuint((int)ctl >> 14)) : tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;   // (cc0x)\n")
    else:   # (2: jr's target fetched behind a branch; 3: fetched always, picked by a select at the end)
        rep("    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;\n",
            "    uint tg = tp + (c0 ? asuint((int)ctl >> 14) : asuint((int)(ctl << 6) >> 23)), tn = tp + 1;   // (cc0x: j's and call's target)\n")
        if P['cc0x'] == 2:
            rep("    uint tk = taken(ctl, CA, CB, i6, sum);\n",
                "    uint tk = taken(ctl, CA, CB, i6, sum);\n    [branch] if (ind) { tg = r[(ctl >> 14) & 63] >> 4; tc = ram(tg); }   // (cc0x 2: jr, callr)\n")
        else:
            rep("    uint tk = taken(ctl, CA, CB, i6, sum);\n",
                "    uint tk = taken(ctl, CA, CB, i6, sum);\n    tg = ind ? ti : tg; tc = ind ? tci : tc;   // (cc0x 3)\n")
            a = "    uint4 nx = ram(tn), tc = ram(tg);\n"
            assert src.count(a) == 1
            src = src.replace(a, a + "    uint ti = r[(ctl >> 14) & 63] >> 4;\n    uint4 tci = ram(ti);   // (cc0x 3: jr's target, fetched always)\n")
    rep("    uint tk = taken(ctl, CA, CB, i6, sum);\n", "    uint tk = c0 ? 1u : taken(ctl, CA, CB, i6, sum);   // (cc0x)\n")
    rep("    r[(ctl & 0x780) != (GVM_CC_ADDBNZ << 7) ? 64 : ca] = sum;\n",
        "    bool ab = (ctl & 0x780) == (GVM_CC_ADDBNZ << 7);\n    r[ab ? ca : cl ? 1 : 64] = ab ? sum : tn << 4;   // (cc0x: the sum, or a call's return address)\n")
    return src

def fstep(P):
    m35 = (xfast(P) if (P['xr'] or P['npair']) and P['xfast'] else '') + (LDF if P.get('ldf', 0) else '') + MEM35
    if P.get('snk', 0):
        a = '''            wt[at0] = stag(slot_tag(m0, sl0), m0.t, m0.i);
            wd[4 * at0 + m0.i] = merge(t.x, w0, m0.ad, r[d0]);
'''
        assert m35.count(a) == 1
        m35 = m35.replace(a, '''            [branch] if (st0) {   // (snk: a load writes no entry)
                wt[at0] = stag(slot_tag(m0, sl0), m0.t, m0.i);
                wd[4 * at0 + m0.i] = merge(t.x, w0, m0.ad, r[d0]);
            }
''')
    m6 = MEM6SEQ if P.get('m6seq', 0) else MEM6
    if P['mem_order'] == '35':
        mem = m35 + ' else ' + MEM4 + ' else ' + m6
    else:
        mem = MEM4 + ' else ' + m35 + ' else ' + m6
    mem = '{\n        ' + mem + '\n    }'
    attr = '[%s] ' % P['split']
    if P['int_first']:
        split = attr + 'if (path == GVM_PATH_INT) ' + INT + ' else ' + mem
    else:
        split = attr + 'if (path != GVM_PATH_INT) ' + mem + ' else ' + INT
    if P['fetch'] == 'both' and P.get('pf', 0):   # (pf: the fall-through's texel nf was asked for an iteration ago)
        pre = '    uint4 tc = ram(tg);\n' if P['pf'] == 1 else ''
        post = ('    [branch] if (tk) { tp = tg; cur = %s; nf = ram(tg + 1); } else { tp = tn; cur = nf; nf = ram(tn + 1); }   // (pf)\n'
                % ('tc' if P['pf'] == 1 else 'ram(tg)'))
        tkpre = ''
        tkpost = '    uint tk = taken(ctl, CA, CB, i6, sum);\n'
    elif P['fetch'] == 'both':
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
    if P.get('dead', 0):   # (dead: never run; _Flags bit 30 is no host's)
        dd = ('    [branch] if ((_Flags & 0x40000000u) != 0) {   // (dead)\n        uint dx = t.x ^ budget;\n'
              '        [unroll] for (uint dk = 0; dk < %du; dk++) dx = (dx * 0x9e3779b1u + dk) ^ (dx >> 7);\n        r[1] = dx;\n    }\n' % P['dead'])
    else: dd = ''
    if P.get('pft', 0):   # (pft: a touch of the code ahead; its result folded into junk an iteration or two later)
        pre += ('    junk ^= pre1; pre1 = ram(tp + %du).x;   // (pft)\n' % P['pft'] if P.get('pfd', 1) == 1 else
                '    junk ^= pre2; pre2 = pre1; pre1 = ram(tp + %du).x;   // (pft, pfd 2)\n' % P['pft'])
    decl = ('static uint4 nf;   // (pf: the fall-through\'s texel, asked for an iteration ahead)\nstatic uint junk, pre1, pre2;   // (pft: the touches)\n'
            if P.get('pf', 0) or P.get('pft', 0) or P.get('xtl', 0) or P.get('lpf', 0) or P.get('lps', 0) else '')
    if P.get('lpf', 0) or P.get('lps', 0): decl += 'static uint lane;   // (lpf, lps: this lane\'s place in the core, 0 to SPIX - 1)\n'
    if dd and P.get('deadat', 0) == 0: pre += dd
    if P.get('cc0x', 0):
        decl += 'static const uint CC0X = 1;   // (cc0x: the fast step makes j, call, jr, callr; the assembler\'s cc0_fast)\n'
    if P.get('xtl', 0):
        pre += ''.join('    junk ^= ram(tp + %du).%s;   // (xtl)\n' % (k + 1, 'xyzw'[k % 4]) for k in range(P['xtl']))
    return cc0x_text(P, decl + '''// ---- The fast loop's step: paths 3-7
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
''' + post + (dd if P.get('deadat', 0) == 1 else '') + '''    budget--;
    ops8 += n8;
}
''')

def fstep_c(P):
    '''cc0f: the fast step's body for a cc 0 bundle (j, call, jr, callr) with path 7's, 3's or 5's ops: the path from its ops,
    the target from the control word (j, call) or rs after the ops (jr, callr: asked for as found, again if an op wrote it),
    ra written last'''
    if not P.get('cc0f', 0): return ''
    src = fstep(dict(P, pf=0, pft=0))
    i = src.index('void fstep(inout uint budget) {')
    src = src[i:].replace('void fstep(inout uint budget) {', 'void fstep_c(inout uint budget) {', 1)
    def rep(a, b):
        nonlocal src
        assert src.count(a) == 1, 'fstep_c: ' + a[:60]
        src = src.replace(a, b)
    rep("    uint ctl = t.w, path = ctl & 7, n8 = ctl & 0x18;\n",
        "    uint ctl = t.w, n8 = ctl & 0x18, o0 = (t.x >> 12) & 255;\n"
        "    uint path = n8 == 0 || (o0 >> 6) != 2 ? GVM_PATH_INT : ((o0 >> 3) & 1) != 0 ? GVM_PATH_STORE : GVM_PATH_LOAD;   // (cc0f: by the ops)\n"
        "    uint sb = (ctl >> 11) & 7, rsx = (ctl >> 14) & 63;\n"
        "    bool ind = sb == GVM_SUB_JR || sb == GVM_SUB_CALLR;\n")
    rep("    uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;\n",
        "    uint tg = ind ? r[rsx] >> 4 : tp + asuint((int)ctl >> 14), tn = tp + 1;\n")
    rep("    uint4 nx = ram(tn), tc = ram(tg);\n", "    uint4 tc = ram(tg);\n")
    rep("    r[s1] = v1;\n", "    r[s1] = v1;\n    [branch] if (ind && rsx != 0 && (s0 == rsx || s1 == rsx)) { tg = r[rsx] >> 4; tc = ram(tg); }   // (an op wrote rs)\n")
    rep("    uint tk = taken(ctl, CA, CB, i6, sum);\n", "")
    rep("    tp = tk ? tg : tn;\n    cur = tk ? tc : nx;\n",
        "    r[sb == GVM_SUB_CALL || sb == GVM_SUB_CALLR ? 1 : 64] = tn << 4;   // (the return address: after the ops)\n    tp = tg;\n    cur = tc;\n")
    return '''bool cc0ok(uint4 c) {   // (cc0f) j, call, jr, callr with no ops or a mid op or a load or store, and a lite op
    uint ctl = c.w, sb = (ctl >> 11) & 7, n = (ctl >> 3) & 3, o0 = (c.x >> 12) & 255, o1 = (c.y >> 12) & 255, f1 = o1 & 31, wd = o0 & 7;
    bool cok = ((ctl >> 7) & 15) == 0 && (sb == GVM_SUB_J || sb == GVM_SUB_CALL || sb == GVM_SUB_JR || sb == GVM_SUB_CALLR) && n < 3 && (ctl & 7) != 0;
    bool m0 = (o0 >> 6) == 1 && (o0 & 31) <= GVM_I_MID_LAST;
    bool mm = (o0 >> 6) == 2 && ((o0 >> 3) & 7) <= 3 && (wd == GVM_W_W || wd == GVM_W_H || wd == GVM_W_B || wd == GVM_W_HU || wd == GVM_W_BU);
    bool l1 = (o1 >> 6) == 1 && (f1 <= GVM_I_XNOR || (f1 >= GVM_I_SLL && f1 <= GVM_I_SRA));
    return cok && (n == 0 || ((m0 || mm) && (n == 1 || l1)));
}
''' + src

def run_text(P):
    src = RUN
    if P.get('cc0f', 0):
        a = "        [branch] if (budget > 0 && (path == GVM_PATH_ALU || path == GVM_PATH_MEMF) && thr == GVM_PATH_FAST) budget = step12(budget);\n"
        assert a in src
        src = src.replace(a, '''        [branch] if (budget > 0 && (path == GVM_PATH_ALU || path == GVM_PATH_MEMF) && thr == GVM_PATH_FAST) {
            [branch] if (cc0ok(cur)) {   // (cc0f)
                uint b1 = budget;
                tp = pc >> 4; ops8 = 0;
                fstep_c(budget);
                cycle += b1 - budget; opsrun += ops8 >> 3; pc = tp << 4;
            } else budget = step12(budget);
        }
''', 1)
    if P.get('pf', 0):
        a = '        tp = pc >> 4;\n'
        assert a in src
        src = src.replace(a, a + '        nf = ram(tp + 1);   // (pf)\n', 1)
    if P.get('lpf', 0) or P.get('lps', 0):   # (lpf, lps: every lane a texel of its own, all asked for at once; folded into junk at the end)
        a = '    cur = ram(pc >> 4);\n'
        assert src.count(a) == 1
        b = ''
        if P.get('lpf', 0):
            b += ('    [unroll] for (uint lk = 0; lk < (%du + SPIX - 1) / SPIX; lk++) junk ^= ram((pc >> 4) - %du + lane + SPIX * lk).y;   // (lpf)\n'
                  % (P['lpf'], P['lpf'] // 4))
        if P.get('lps', 0):
            b += ('    [unroll] for (uint ls = 0; ls < (%du + SPIX - 1) / SPIX; ls++) { uint t = (r[2] >> 4) + lane + SPIX * ls; junk ^= _Ram.Load(int3(t %% RAM_W, (t / RAM_W) & 0x1fff, 0)).y; }   // (lps)\n'
                  % P['lps'])
        src = src.replace(a, a + b, 1)
    if P.get('pft', 0) or P.get('xtl', 0) or P.get('lpf', 0) or P.get('lps', 0):
        a = '        else [branch] if (budget > 0) budget = general(budget);\n    }\n'
        assert a in src
        src = src.replace(a, a + '    cycle += junk == _TimeLo + 0x9e3779b9u && _NCores == 0 ? 1u : 0u;   // (pft: the touches kept; never true while a core runs)\n', 1)
    return src

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
    uint4 s19 = uint4(cycle, opsrun, cycle - s3.x, endr == E_BUDGET && cycle - s3.x < bud && (flags & F_HALT) == 0 ? E_FULL : endr);
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

WADBLK = "    bud = core == 0 ? _Ticks : _Ticks >> WSH;   // (wsh)\n    [branch] if (WAD != 0 && core != 0) {   // (wad: the pass ends about when core 0's does)\n        uint4 z = state_in(19, 0);\n        bool cap = WAD == 1 || WAD == 4 ? z.w == E_SYNC || z.w == E_FULL : WAD != 5 && z.w == E_FULL;   // (2, 3, 6: core 0 had work: its cache was full; 5 never then)\n        [branch] if (WAD == 7 && cap) cap = ram((WAITW >> 4) + 2).x != 0;   // (7: on full only while a program has declared its mask)\n        [branch] if (WAD >= 5 && z.w == E_SYNC) {   // (5-7: core 0 rested: the workers it waits for, from the kernel's word at WAITW; no mask, no cap)\n            uint4 m0 = ram(WAITW >> 4), m1 = ram((WAITW >> 4) + 1);\n            uint4 mm = (core >> 7) != 0 ? m1 : m0;\n            bool given = any((m0 | m1) != 0), mine = ((pick(mm, (core >> 5) & 3) >> (core & 31)) & 1) != 0;\n            cap = given && !mine;\n        }\n        [branch] if (WAD == 4 && cap) {   // (4: not when the workers are the work: half of cores 1-8 ran their whole budget)\n            uint busy = 0, k;\n            [loop] for (k = 1; k <= 8; k++) busy += k < _NCores && state_in(19, k).w == E_BUDGET ? 1u : 0u;\n            cap = busy < 4;\n        }\n        bud = cap ? clamp(z.z + (z.z >> 2), WAD == 3 ? bud >> 1 : 512u, bud) : bud;\n    }\n"   # (the frag's budget: wsh, wad)

def wad_text(P):
    t = WADBLK
    if P.get('wadf', 0) & 1:   # (straight-line: every core reads the words, no branches)
        t = t.replace('[branch] if (WAD != 0 && core != 0) {', '{').replace('[branch] if (WAD', '[flatten] if (WAD')
    return t
def wad_top(P): return wad_text(P) if P.get('wadf', 0) & 8 else ''
def wad_pre(P): return wad_text(P) if (P.get('wadf', 0) & 10) == 2 else ''
def wad_post(P): return '' if P.get('wadf', 0) & 10 else wad_text(P)

def wclean(P):
    """wclean 1: a pixel of the write cache's data texels whose eight entries are all empty is not written (discarded): its
    texels keep what they held, which nothing reads (the commit draws an entry by its tag; the next pass clears the tags)"""
    if not P.get('wclean', 0): return ''
    return """    [branch] if (j * 8 >= T_WC && j * 8 + 7 < T_WT) {   // (wclean)
        uint e0 = j * 8 - T_WC, used = 0, k;
        [unroll] for (k = 0; k < 8; k++) used |= wt[e0 + k] != NONE ? 1u : 0u;
        [branch] if (used == 0) discard;
    }
"""

def fw_park():   # the worker firmware's park loop, [GVM_FW_PARK, GVM_FW_PARK_END) (gpu_vm/platform/worker_fw.h)
    import re
    fw = open(os.path.join(HERE, '..', '..', 'platform', 'worker_fw.h')).read()
    return tuple(int(re.search(r'#define %s (0x[0-9a-fA-F]+)u' % k, fw).group(1), 16) for k in ('GVM_FW_PARK', 'GVM_FW_PARK_END'))

def frag(P):
    bw, bh, band, per = P['bw'], P['bh'], P['band'], P['per']
    j = ('ry * %du + rx' % bw) if P['jrow'] else ('rx * %du + ry' % bh)
    early = ''
    if P.get('idlew', 0):   # (idlew) RISC-V's mc_idle: a parked worker whose start word is as it was costs a copy, not a pass
        assert not P['npair'], 'idlew: npair 0 (texel 23)'
        park, end = fw_park()
        early += '''    [branch] if (core != 0 && ext == 0) {   // (idlew) parked in the firmware (worker_fw.h's park loop), its start texel as it was
        uint4 s16 = state_in(16, core), s19 = state_in(19, core), s22 = state_in(22, core);
        [branch] if (s16.x >= 0x%xu && s16.x < 0x%xu && s19.w == E_SYNC && s22.x == 0 && all(ram((0x00c00000u >> 4) + core) == state_in(23, core))) {
            uint4 os = _OpState.Load(int3(core, 0, 0));
            bool fold = os.x == s22.z;
            uint4 n22 = uint4(0, fold ? os.z : s22.y, s22.z + 1, s22.w + (fold ? os.w : 0));
            uint4 v[8];
            [unroll] for (uint k = 0; k < 8; k++) {
                uint t = j * 8 + k;
                uint4 x = t < T_WC ? state_in(t, core) : t < T_WT ? uint4(0, 0, 0, 0) : t < T_XF ? uint4(NONE, NONE, NONE, NONE) : uint4(0, 0, 0, 0);
                x = t == 18 ? uint4(x.xy, x.z & ~F_AGAIN, x.w) : t == 19 ? uint4(x.xy, 0, E_SYNC) : t == 22 ? n22 : t == 23 ? state_in(23, core) : x;
                v[k] = x;
            }
            o.t0 = v[0]; o.t1 = v[1]; o.t2 = v[2]; o.t3 = v[3]; o.t4 = v[4]; o.t5 = v[5]; o.t6 = v[6]; o.t7 = v[7];
            return o;
        }
    }
''' % (park, end)
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
''' + wad_top(P) + early + wad_pre(P) + '''    load_state(core);
    thr = (flags & F_AGAIN) != 0 || qn != 0 ? GVM_PATH_INT : GVM_PATH_FAST;
    flags &= ~F_AGAIN;
    lmax = 0; smax = 0;
''' + ('    lane = j;   // (lpf, lps)\n' if P.get('lpf', 0) or P.get('lps', 0) else '') + wad_post(P) + ('    [branch] if (core == 0) run(_Ticks); else run(bud);   // (wadf 4: core 0 runs a copy whose budget is the constant)@N' if P.get('wadf', 0) & 4 else '    run(core == 0 ? _Ticks : bud);@N' if P.get('wadf', 0) & 16 else '    run(bud);@N').replace('@N', chr(10)) + '''    bool again = lmax >= _RamEnd || smax >= _RamEnd - GVM_DEV;
''' + wclean(P) + '''    uint4 s19 = state_in(19, core);
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

def general(P):
    return part('general').replace('//@@FILE_ANY@@', file_any(P))

def step12(P):
    """step12 (paths 1 and 2): with the assembler's file_paths a file op with a cc 0 control word (call, ret, jr) is path 2's"""
    src = part('step12')
    if P.get('s12pf', 0):   # (s12pf: the fall-through and the target, j's, call's or jr's as rs was found, asked for before the ops)
        a = "    uint opc0 = (t.x >> 12) & 255, C0 = r[GVM_RC(t.x)], cs0 = r[GVM_CRS(ctl)];\n"
        assert src.count(a) == 1
        src = src.replace(a, a + r'''    uint sb0 = GVM_SUB(ctl), jt = GVM_CC(ctl) != GVM_CC_SPECIAL ? tg : sb0 == GVM_SUB_J || sb0 == GVM_SUB_CALL ? base + asuint(GVM_OFF18(ctl)) * 16
            : sb0 == GVM_SUB_JR || sb0 == GVM_SUB_CALLR ? cs0 & ~15u : base + 16;
    uint4 qf = ram((base >> 4) + 1), qt = ram(jt >> 4);   // (s12pf)
''')
        a = "    cur = ram(pc >> 4);\n    cycle++;"
        assert src.count(a) == 1
        src = src.replace(a, "    cur = pc == base + 16 ? qf : qt;   // (s12pf)\n    [branch] if (pc != base + 16 && pc != jt) cur = ram(pc >> 4);\n    cycle++;")
    if P.get('mv', 0):   # (mv: a store's lookup may move a resident)
        a = "        mop m = mread(t.x, i0, A0, B0);"
        assert src.count(a) == 1
        src = src.replace(a, "        mvok = (t.x & op_bit(8)) != 0; mop m = mread(t.x, i0, A0, B0); mvok = false;")
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
    rep(" : t == 23 ? uint4(pn, 0, 0, 0) : 0;", " : t == 23 ? (core != 0 && pc >= 0x%xu && pc < 0x%xu && endr == E_SYNC ? ram((0x00c00000u >> 4) + core) : uint4(NONE, NONE, NONE, NONE)) : 0;   // (idlew: the start texel a parked pass saw)" % fw_park()
        if P.get('idlew', 0) else " : 0;")
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

def core0(P, src):
    """c0nb: core 0's own write cache of 2 x c0nb buckets (8 * c0nb + 2 entries), its tick the same source compiled with
    CORE0=1 and drawn apart (the harness's and gvmbench's x25 path): its state pixels from bw * bh on lie in the rows under its
    block (a block's rows again, as many as it needs), the workers' blocks are as they were. Only the core-0 compile and the
    commit's (and the operations') reads of core 0's state differ from the point without it."""
    import re
    n0 = P.get('c0nb', 0)
    if not n0: return src
    bw, bh, band = P['bw'], P['bh'], P['band']
    assert P['jrow'] == 0 and P['perm'] == 'id' and not P['xr'] and not P['npair'] and not P.get('vec', 0) and not P.get('ov', 0), 'c0nb: a plain point'
    assert P['hash'] == 1, 'c0nb: hash=1'
    blk = bw * bh
    L = layout(P)
    w0 = 8 * n0 + 2
    nt0 = L['T_WC'] + w0 + (w0 + 3) // 4
    sp0 = (nt0 + 7) // 8
    rows = (sp0 + blk - 1) // blk * bh
    assert rows <= band, 'c0nb: core 0 needs %d rows of a band of %d' % (rows, band)
    def rep(a, b):
        nonlocal src
        assert src.count(a) == 1, 'core0: ' + a[:70]
        src = src.replace(a, b)
    def two(prefix, v0):   # a constant's line: the workers' value, core 0's in its compile
        line = src[src.index(prefix):].split('\n', 1)[0]
        name = re.match(r'static const uint (\w+) = ', line).group(1)
        rep(line + '\n', '#if !CORE0\n' + line + '\n#else\nstatic const uint %s = %s;\n#endif\n' % (name, v0))
    i = src.index('\n') + 1
    src = src[:i] + ('#ifndef CORE0\n#define CORE0 0   // 1: core 0\'s tick (its own compile and draw: c0nb)\n#endif\n'
                     'static const uint WCN0 = %d;   // core 0\'s write cache entries (8 * %d + 2) and state texels\n'
                     'static const uint NTEX0 = %d;\n'
                     'static const uint C0BLK = %du, C0BH = %du;   // core 0\'s pixel j from C0BLK on: C0BH rows down, a block again\n'
                     % (w0, n0, nt0, blk, bh)) + src[i:]
    two('static const uint NB = ', n0)
    two('static const uint WCN = ', w0)
    two('static const uint NTEX = ', nt0)
    two('static const uint SPIX = ', sp0)
    for t in ('T_XF', 'T_XT', 'T_PB', 'T_VR'):
        two('static const uint %s = ' % t, nt0)
    # the cache's arrays as core 0's in both compiles: two pixel shaders of different local sizes do not overlap on the
    # card (the workers' draws waited for core 0's: Doom 220 ns a worker bundle on top of core 0's, 72 with one size)
    def c0size(m):   # (an array the cache sizes: its length from core 0's entries, whatever the layout)
        n = re.sub(r'\bNBK\b', '((WCN0 + 4) / 4)', re.sub(r'\bWCN\b', 'WCN0', m.group(2)))
        return m.group(1) + n + m.group(3) + ('   // (WCN0: one local size for both compiles)' if 'wt' in m.group(1) else '')
    src, k = re.subn(r'^(static uint4? \w+\[)([^\]\n]*\b(?:WCN|NBK)\b[^\]\n]*)(\];)', c0size, src, flags=re.M)
    assert k, 'core0: no cache arrays'
    def hashes(nb):   # (a power of two: the shift; else the high part of a 16-bit fraction)
        if nb & (nb - 1) == 0:
            lg = nb.bit_length() - 1
            return ('uint wc_b1(uint t) { return (t * 0x9e3779b1u) >> %d; }\n'
                    'uint wc_b2(uint t) { return NB + ((t * 0x85ebca6bu + 0x165667b1u) >> %d); }\n' % (32 - lg, 32 - lg))
        return ('uint wc_b1(uint t) { return (((t * 0x9e3779b1u) >> 16) * NB) >> 16; }\n'
                'uint wc_b2(uint t) { return NB + ((((t * 0x85ebca6bu + 0x165667b1u) >> 16) * NB) >> 16); }\n')
    old = hashes(P['nb'])
    rep(old, '#if !CORE0\n' + old + '#else\n' + hashes(n0) + '#endif\n')
    old = src[src.index('int3 sat(uint j, uint c) {'):].split('\n', 1)[0]
    c0 = 'int3((j % C0BLK) / C0BH, (j / C0BLK) * C0BH + (j % C0BLK) % C0BH, 0)'
    rep(old + '\n', '#if !CORE0\n' + old + '\n#else\nint3 sat(uint j, uint c) { return ' + c0 + '; }   // (core 0)\n#endif\n'
        'int3 satx(uint j, uint c) { return c == 0 ? ' + c0 + ' : sat(j, c); }   // (the commit\'s and operations\' reads: core 0 too)\n')
    a = 'uint4 state_any(uint t, uint c) {\n    int3 at = sat(t >> 3, c);'
    rep(a, a.replace('sat(', 'satx('))
    a = ('    core = (p.y / %du) * %du + core_at(p.x / %du);\n    uint j = rx * %du + ry;\n'
         '    [branch] if (core >= _NCores || j >= SPIX || ry >= %du) {' % (band, P['per'], bw, bh, bh))
    rep(a, '#if CORE0\n    core = 0;\n    uint j = (p.y / C0BH) * C0BLK + rx * C0BH + p.y % C0BH;\n'
           '    [branch] if (p.x >= ' + str(bw) + 'u || p.y >= ' + str(rows) + 'u || j >= SPIX) {\n#else\n' + a + '\n#endif\n')
    a = '    uint c = id / WCN, j = id % WCN;\n    uint tg = pick(state_any(T_WT + (j >> 2), c), j & 3)'
    rep(a, '    bool z = id < WCN0;\n    uint k = id - WCN0, c = z ? 0 : 1 + k / WCN, j = z ? id : k % WCN;\n'
           '    uint tg = pick(state_any(T_WC + (z ? WCN0 : WCN) + (j >> 2), c), j & 3)')
    return core0_rt(P, src, rows) if P.get('c0rt', 0) else src

def core0_rt(P, src, rows):
    """c0rt: core 0's cache chosen at run time in the one compile, as RISC-V's tick sizes each core's cache by its geometry
    (mc_bits_now): every core in one draw, core 0 the largest. The CORE0 pairs core0() wrote become choices on `core == 0`,
    what is made of them follows, the hashes take the core's buckets, a pixel in core 0's rows of the first block is core 0's."""
    import re
    names, worker = [], {}
    def pair(m):   # '#if !CORE0 / static const uint X = a; / #else / static const uint X = b; / #endif' -> a choice
        names.append(m.group(1)); worker[m.group(1)] = m.group(2)
        return '#define %s\t(core == 0u ? %su : %su)\n' % (m.group(1), m.group(3), m.group(2))   # (a tab: the hosts read the 'uint X = ' below)
    src = re.sub(r'#if !CORE0\nstatic const uint (\w+) = (\w+);[^\n]*\n#else\nstatic const uint \1 = (\w+);[^\n]*\n#endif\n', pair, src)
    src = src.replace('static const uint OV = 0, OVB = 8 * NB;', 'static const uint OV = 0;\n#define OVB (8u * NB)')
    a = '    uint k = id - WCN0, c = z ? 0 : 1 + k / WCN, j = z ? id : k % WCN;\n    uint tg = pick(state_any(T_WC + (z ? WCN0 : WCN) + (j >> 2), c), j & 3)'
    assert src.count(a) == 1, 'c0rt: the commit'   # (the commit's `core` is no core's: the workers' count written out)
    src = src.replace(a, a.replace('WCN;', worker['WCN'] + 'u;').replace('k / WCN', 'k / %su' % worker['WCN']).replace(': WCN)', ': %su)' % worker['WCN']))
    while True:   # a constant made of a choice is one too
        m = re.search(r'^static const uint (\w+) = ([^;\n]*\b(?:%s)\b[^;\n]*);' % '|'.join(names), src, flags=re.M)
        if not m: break
        names.append(m.group(1))
        src = src[:m.start()] + '#define %s\t(%s)' % (m.group(1), m.group(2)) + src[m.end():]
    m = re.search(r'#if !CORE0\nuint wc_b1[^\n]*\nuint wc_b2[^\n]*\n#else\n(uint wc_b1[^\n]*\nuint wc_b2[^\n]*\n)#endif\n', src)
    assert m, 'c0rt: the hashes'
    src = src[:m.start()] + m.group(1) + src[m.end():]   # (the high part of a 16-bit fraction: any core's count)
    m = re.search(r'#if !CORE0\nint3 sat\(uint j, uint c\) \{ return ([^;]*); \}\n#else\nint3 sat\(uint j, uint c\) \{ return ([^;]*); \}[^\n]*\n#endif\n', src)
    assert m, 'c0rt: sat'
    src = src[:m.start()] + 'int3 sat(uint j, uint c) { return c == 0 ? %s : %s; }   // (core 0 in the rows under its block)\n' % (m.group(2), m.group(1)) + src[m.end():]
    m = re.search(r'#if CORE0\n    core = 0;\n    uint j = ([^;]*);\n    \[branch\] if \(([^{]*)\) \{\n#else\n    core = ([^;]*);\n    uint j = ([^;]*);\n'
                  r'    \[branch\] if \(([^{]*)\) \{\n#endif\n', src)
    assert m, 'c0rt: the pixel'
    src = src[:m.start()] + ('    core = %s;\n    uint j = core == 0u ? %s : %s;   // (c0rt: core 0\'s pixels in its rows of the first block)\n'
                             '    [branch] if (core == 0u ? p.y >= %du || j >= SPIX : (%s)) {\n' % (m.group(3), m.group(1), m.group(4), rows, m.group(5))) + src[m.end():]
    assert 'CORE0\n' not in src.replace('#ifndef CORE0\n', '').replace('#define CORE0 0', ''), 'c0rt: a CORE0 left'
    src = src.replace('#ifndef CORE0\n#define CORE0 0   // 1: core 0\'s tick (its own compile and draw: c0nb)\n#endif\n',
                      'static const uint C0RT = 1;   // core 0\'s cache chosen at run time: one compile, one draw (the harness: rows C0ROWS of the first band)\n'
                      'static const uint C0ROWS = %d;\n' % rows + '#if 0   // (for the hosts, which read a size as text: a worker\'s)\n' +
                      ''.join('static const uint %s = %s;\n' % (k, v) for k, v in worker.items()) + '#endif\n')
    return src

def emit(P):
    P = full(P)
    why = valid(P)
    if why: raise ValueError(why)
    L = layout(P)
    out = [header(P, L), part('alu'), memory(P), part('taken'), fstep(P), step12(P), general(P)] + ([fstep_c(P)] if P.get('cc0f', 0) else []) + [run_text(P),
           state_io(P), frag(P), part('ops')]
    from memopts import memopts
    from layoutopts import layoutopts
    return rmap_text(P, core0(P, layoutopts(P, sil_text(P, memopts(P, mflat(P, evf_text(P, p2strip(P, '\n'.join(out)))))))))

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
    args = sys.argv[2:]   # name=value ..., or a manifest (its params) and then name=value over them
    P = json.load(open(args.pop(0)))['params'] if args and args[0].endswith('.json') else {}
    P.update(parse_kv(args))
    src = emit(P)
    isa = os.path.normpath(os.path.join(HERE, '..', '..', 'isa.h'))
    root, out = os.path.normcase(os.path.dirname(os.path.dirname(os.path.dirname(isa)))), os.path.abspath(sys.argv[1])
    if os.path.normcase(out).startswith(root + os.sep):   # a tick inside the tree (a step of gpu_vm/tick/steps, say) includes isa.h by a relative path
        rel = os.path.relpath(isa, os.path.dirname(out))
        src = src.replace('#include "%s"' % isa.replace(os.sep, '/'), '#include "%s"' % rel.replace(os.sep, '/'), 1)
    with open(sys.argv[1], 'w', newline='\n') as f: f.write(src)
    m = manifest(P)
    with open(os.path.splitext(sys.argv[1])[0] + '.json', 'w') as f: json.dump(m, f, indent=1)
    print(json.dumps(m))
