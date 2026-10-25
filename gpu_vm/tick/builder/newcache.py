"""The new tick's write cache and commit (gpu_vm/tick/CACHE.md, "Decided for the new tick"), as parameters of the builder:

    probe=N   one array of NB buckets of four, probed bucket after bucket from the texel's own to the first that holds the
              texel or has a free way, N buckets at most (0: the two tables). Entries are only added in a pass and the cache
              is cleared at its start, so the first free way ends a search exactly.
    bmap=B    a load looks up only when the texel's bit of a B-bit map of the pass's stored texels is set (0: the OR word)
    wtx=1     an entry is the whole texel: the words the core did not store are RAM's, filled at the pass's end; the commit is
              one draw of whole texels (COMMIT_WHOLE), and a load whose word the cache holds fetches no texel
"""
import math
import re


def probe_memory(P):
    """mfind and mlook for probe: replaces the two tables' (memory()'s text between bucket() and mread_entries)"""
    nb = P['nb']
    if nb & (nb - 1):
        h = 'uint wc_b1(uint t) { return (((t * 0x9e3779b1u) >> 16) * NB) >> 16; }'
    else:
        h = 'uint wc_b1(uint t) { return (t * 0x9e3779b1u) >> %d; }' % (32 - int(math.log2(nb)))
    return h + '''
uint wc_b2(uint t) { return 0; }
uint4 bucket(uint b) { uint k = 4 * b; return uint4(wt[k], wt[k + 1], wt[k + 2], wt[k + 3]); }
struct mop { uint ad, t, i, b1, b2; uint4 q, A, B; uint sl, otag, e, w; };
static const uint PROBE = %d;   // (probe) buckets a lookup reads at most
@MFIND@mop mlook(uint ad, bool look) {
    mop m;
    m.ad = ad; m.t = ad >> 4; m.i = (ad >> 2) & 3; m.b1 = wc_b1(m.t); m.b2 = 0;
    m.q = 0; m.A = NONE; m.B = NONE; m.otag = NONE; m.sl = NONE; m.e = WCN;
    [branch] if (look) mfind(m);
    m.w = wd[4 * m.e + m.i];
    return m;
}
'''.replace('@MFIND@', MFIND[P.get('probef', 0)]) % P['probe']


MFIND = {
    0: """void mfind(inout mop m) {   // (probe) bucket after bucket from the texel's own, to the first that holds it or has a free way
    uint b = m.b1, n;
    m.otag = NONE; m.sl = NONE;
    [loop] for (n = 0; n < PROBE; n++) {
        uint4 A = bucket(b);
        bool4 k = (A & 0x0fffffffu) == m.t || A == NONE;
        uint j = k.x ? 0u : k.y ? 1u : k.z ? 2u : k.w ? 3u : 4u;
        [branch] if (j < 4) { m.otag = pick(A, j); m.sl = 4 * b + j; break; }
        b = b + 1 == NB ? 0u : b + 1;
    }
    m.e = has(m.otag, m.t, m.i) ? m.sl : WCN;
}
""",
    1: """void mfind(inout mop m) {   // (probe, probef 1) the texel's own bucket straight; the next ones in a loop only when it is full of others
    uint b = m.b1, n;
    uint4 A = bucket(b);
    bool4 k = (A & 0x0fffffffu) == m.t || A == NONE;
    uint j = k.x ? 0u : k.y ? 1u : k.z ? 2u : k.w ? 3u : 4u;
    [branch] if (j >= 4) {
        [loop] for (n = 1; n < PROBE && j >= 4; n++) {
            b = b + 1 == NB ? 0u : b + 1;
            A = bucket(b);
            k = (A & 0x0fffffffu) == m.t || A == NONE;
            j = k.x ? 0u : k.y ? 1u : k.z ? 2u : k.w ? 3u : 4u;
        }
    }
    m.otag = j < 4 ? pick(A, j) : NONE; m.sl = j < 4 ? 4 * b + j : NONE;
    m.e = has(m.otag, m.t, m.i) ? m.sl : WCN;
}
""",
    2: """void mfind(inout mop m) {   // (probe, probef 2) the texel's bucket and the next read together, a first free way in either; further in a loop
    uint b = m.b1, b2 = b + 1 == NB ? 0u : b + 1, n;
    uint4 A = bucket(b), B = bucket(b2);
    bool4 ka = (A & 0x0fffffffu) == m.t || A == NONE, kb = (B & 0x0fffffffu) == m.t || B == NONE;
    uint ja = ka.x ? 0u : ka.y ? 1u : ka.z ? 2u : ka.w ? 3u : 4u, jb = kb.x ? 0u : kb.y ? 1u : kb.z ? 2u : kb.w ? 3u : 4u;
    uint j = ja < 4 ? ja : jb;
    A = ja < 4 ? A : B; b = ja < 4 ? b : b2;
    [branch] if (j >= 4) {
        [loop] for (n = 2; n < PROBE && j >= 4; n++) {
            b = b + 1 == NB ? 0u : b + 1;
            A = bucket(b);
            ka = (A & 0x0fffffffu) == m.t || A == NONE;
            j = ka.x ? 0u : ka.y ? 1u : ka.z ? 2u : ka.w ? 3u : 4u;
        }
    }
    m.otag = j < 4 ? pick(A, j) : NONE; m.sl = j < 4 ? 4 * b + j : NONE;
    m.e = has(m.otag, m.t, m.i) ? m.sl : WCN;
}
""",
}


def _rep(src, a, b, n=1):
    assert src.count(a) == n, ('newcache', a, src.count(a))
    return src.replace(a, b)


def newcache(P, src):
    if P.get('probe', 0):
        i = src.index('uint4 bucket(uint b) {')
        j = src.index('mop mread_entries(uint w, uint imm, uint a, uint b, bool st) {')
        k = src.index('uint wc_b1(uint t)')
        l = src.index('\n', src.index('uint wc_b2(uint t)')) + 1
        assert k < l <= i < j
        src = src[:k] + src[l:i] + probe_memory(P) + src[j:]
    if P.get('bmap', 0):
        bits = P['bmap']
        lg = int(math.log2(bits))
        decl = ('static const uint BMW = %d;   // (bmap) the pass\'s stored texels, a bit a hash of each\n'
                'static uint bmw[BMW];\n'
                'uint bm_bit(uint t) { return (t * 0x85ebca6bu + 0xc2b2ae35u) >> %d; }\n'
                'void bnote(uint t) { uint h = bm_bit(t); bmw[h >> 5] |= 1u << (h & 31); }\n'
                'bool bhas(uint t) { uint h = bm_bit(t); return ((bmw[h >> 5] >> (h & 31)) & 1) != 0; }\n') % (bits // 32, 32 - lg)
        reg = P.get('bmr', 0)
        if reg:   # (bmr) the map in registers: two uint4, a word picked by compares
            assert bits in (128, 256), 'bmr: 128 or 256 bits'
            decl = ('static uint4 bm0, bm1;   // (bmap, bmr) the pass\'s stored texels, a bit a hash of each, in registers\n'
                    'uint bm_bit(uint t) { return (t * 0x85ebca6bu + 0xc2b2ae35u) >> %d; }\n'
                    'void bnote(uint t) { uint h = bm_bit(t), w = h >> 5, f = 1u << (h & 31);\n'
                    '    bm0 |= f * uint4(w == 0, w == 1, w == 2, w == 3); bm1 |= f * uint4(w == 4, w == 5, w == 6, w == 7); }\n'
                    'bool bhas(uint t) { uint h = bm_bit(t); return ((pick((h >> 7) != 0 ? bm1 : bm0, (h >> 5) & 3) >> (h & 31)) & 1) != 0; }\n') % (32 - lg)
        src = _rep(src, 'static uint bloom;\n', decl)
        src = _rep(src, 'bloom |= m0.t | (st1 ? m1.t : 0);', 'bnote(m0.t); [branch] if (st1) bnote(m1.t);', src.count('bloom |= m0.t | (st1 ? m1.t : 0);'))
        src = re.sub(r'bloom \|= (\w+) \? ([\w.]+) : 0;', r'[branch] if (\1) bnote(\2);', src)
        src = re.sub(r'bloom \|= ([\w.]+);', r'bnote(\1);', src)
        src = _rep(src, '    bloom = 0;\n', '    bm0 = 0; bm1 = 0;\n' if reg else '    [unroll] for (uint bk = 0; bk < BMW; bk++) bmw[bk] = 0;\n')
        src = src.replace('((ad >> 4) & bloom) == (ad >> 4)', 'bhas(ad >> 4)')
        assert 'bloom' not in re.sub(r'//[^\n]*', '', src), 'bmap: a use of the OR word left'
    if P.get('wtx', 0):
        assert not P['xr'] and not P['npair'], 'wtx: no file, no pairs (their points write a word)'
        src = _rep(src, 'static const uint CPTS =', 'static const uint COMMIT_WHOLE = 1;   // (wtx) an entry\'s whole texel, one commit draw\nstatic const uint CPTS =')
        src = _rep(src, '''    [branch] if (t >= T_WC && t < T_WT) { uint e = 4 * (t - T_WC); return uint4(wd[e], wd[e + 1], wd[e + 2], wd[e + 3]); }''',
                   '''    [branch] if (t >= T_WC && t < T_WT) {   // (wtx) the words the core did not store are RAM's
        uint e = 4 * (t - T_WC), g = wt[t - T_WC];
        uint4 v = uint4(wd[e], wd[e + 1], wd[e + 2], wd[e + 3]);
        [branch] if (g != NONE && (g >> 28) != 15) {
            uint4 m = ram(g & 0x0fffffffu);
            v = uint4((g & 0x10000000u) ? v.x : m.x, (g & 0x20000000u) ? v.y : m.y, (g & 0x40000000u) ? v.z : m.z, (g & 0x80000000u) ? v.w : m.w);
        }
        return v;
    }''')
        src = _rep(src, 'struct c2f { float4 vertex : SV_POSITION; nointerpolation uint value : V; };',
                   'struct c2f { float4 vertex : SV_POSITION; nointerpolation uint4 value : V; };')
        src = _rep(src, '''    o.vertex = tg != NONE && ((tg >> (28 + _Comp)) & 1) != 0 ? float4''', '''    o.vertex = tg != NONE ? float4''')   # (wtx) the whole texel
        src = _rep(src, '''    o.value = pick(state_any(T_WC + j, c), _Comp);''', '''    o.value = state_any(T_WC + j, c);''')
    if P.get('wtx', 0) >= 2:   # a hit reads no RAM: a texel fetched only when the cache does not hold the word
        src = _rep(src, '    m.q = data_texel(m.ad);\n    return m;\n', '    [branch] if (m.e == WCN) m.q = data_texel(m.ad);   // (wtx) a hit reads no RAM\n    return m;\n', 2)
        src = _rep(src, '[branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);',
                   '[branch] if (m0.e == WCN && (((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);')
        if P.get('m6seq', 0):
            src = _rep(src, '[branch] if (((t.x & op_bit(3)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);',
                       '[branch] if (m0.e == WCN && ((t.x & op_bit(3)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);')
            src = _rep(src, '[branch] if ((((t.y ^ op_bit(8)) & op_bit(11)) | (m1.ad & 3)) != 0) m1.q = data_texel(m1.ad);',
                       '[branch] if (m1.e == WCN && (((t.y ^ op_bit(8)) & op_bit(11)) | (m1.ad & 3)) != 0) m1.q = data_texel(m1.ad);')
    return src
