"""x24's memory-op ideas as transformations of a composed tick's text (gen.py's mru, bb, tfirst, s6mru). Each is a
replacement of text gen.py emits; an assertion fails when the text it expects is not there."""

NL = "\n"

def _rep(src, a, b, need=True):
    if need: assert a in src, 'memopts: no ' + a[:60]
    return src.replace(a, b, 1)

def memopts(P, src):
    N = P.get('mru', 0)
    if P.get('bb', 0):
        decl = 'static uint bb0, bb1;   // the first-table buckets stored to (x24\'s filter)' + NL
        decl += 'void bbnote(uint t) { uint b = wc_b1(t); [flatten] if (b < 32) bb0 |= 1u << b; else bb1 |= 1u << (b & 31); }' + NL
        decl += 'bool bbhas(uint t) { uint b = wc_b1(t); return (((b < 32 ? bb0 : bb1) >> (b & 31)) & 1) != 0; }' + NL
        src = _rep(src, 'uint4 bucket(uint b) {', decl + 'uint4 bucket(uint b) {')
        src = _rep(src, 'bloom |= st0 ? m0.t : 0;', 'bloom |= st0 ? m0.t : 0; [branch] if (st0) bbnote(m0.t);')
        src = _rep(src, 'bloom |= m0.t | (st1 ? m1.t : 0);', 'bloom |= m0.t | (st1 ? m1.t : 0); bbnote(m0.t); [branch] if (st1) bbnote(m1.t);')
        src = _rep(src, 'bloom |= stv ? m.t : 0;', 'bloom |= stv ? m.t : 0; [branch] if (stv) bbnote(m.t);')
        src = _rep(src, '            bloom |= t;' + NL, '            bloom |= t; bbnote(t);' + NL)
        src = _rep(src, '    bloom = 0;' + NL, '    bloom = 0; bb0 = 0; bb1 = 0;' + NL)
        if 'return mlook(ad, st || ((ad >> 4) & bloom) == (ad >> 4));' in src:
            src = _rep(src, 'return mlook(ad, st || ((ad >> 4) & bloom) == (ad >> 4));',
                       'return mlook(ad, st || (((ad >> 4) & bloom) == (ad >> 4) && bbhas(ad >> 4)));')
        else:
            src = _rep(src, 'return mlook(ad, true);', 'return mlook(ad, st || bbhas(ad >> 4));')
    if N:
        decl = 'static uint4 mrt, mrs;   // the last stored texels (texel + 1, 0: none) and their entries, the latest first (x24)' + NL
        decl += 'void mru_note(uint t, uint sl) { [flatten] if (mrt.x != t + 1) { mrt = uint4(t + 1, mrt.xyz); mrs = uint4(sl, mrs.xyz); } }' + NL
        src = _rep(src, 'mop mlook(uint ad, bool look) {', decl + 'mop mlook(uint ad, bool look, bool mr) {')
        comp = ['x', 'y', 'z', 'w'][:N]
        hit = ' || '.join('mrt.%s == m.t + 1' % c for c in comp)
        sel = 'mrs.%s' % comp[-1]
        for c in reversed(comp[:-1]):
            sel = '(mrt.%s == m.t + 1 ? mrs.%s : %s)' % (c, c, sel)
        old = """    [branch] if (look) {
        m.A = bucket(m.b1); m.B = bucket(m.b2);
    }
    mfind(m);"""
        new = """    [branch] if (mr && (""" + hit + """)) {   // a texel stored lately: its entry without the tags
        uint sl = """ + sel + """;
        m.sl = sl; m.otag = wt[sl]; m.e = has(m.otag, m.t, m.i) ? sl : WCN;
    } else {
        [branch] if (look) {
            m.A = bucket(m.b1); m.B = bucket(m.b2);
        }
        mfind(m);
    }"""
        src = _rep(src, old, new)
        src = src.replace('mlook(wi << 2, true)', 'mlook(wi << 2, true, true)').replace('mlook(a, true)', 'mlook(a, true, true)')
        # mread_entries takes the kept texels (paths 3, 5, step12); path 6's lookups do not (its second op's buckets are patched)
        src = _rep(src, 'mop mread_entries(uint w, uint imm, uint a, uint b, bool st) {',
                   'mop mread_entries2(uint w, uint imm, uint a, uint b, bool st, bool mr) {')
        src = _rep(src, '    return mlook(ad, ', '    return mlook2(ad, mr, ')
        src = _rep(src, 'mop mread_load(uint w',
                   'mop mread_entries(uint w, uint imm, uint a, uint b, bool st) { return mread_entries2(w, imm, a, b, st, true); }' + NL + 'mop mread_load(uint w')
        src = _rep(src, 'mop mread_entries2(', 'mop mlook2(uint ad, bool mr, bool look) { return mlook(ad, look, mr); }' + NL + 'mop mread_entries2(')
        src = _rep(src, 'mop m0 = mread_entries(t.x, i0, A0, B0, true), m1 = mread_entries(t.y, i1, A1, B1, true);',
                   'mop m0 = mread_entries2(t.x, i0, A0, B0, true, false), m1 = mread_entries2(t.y, i1, A1, B1, true, false);')
        src = _rep(src, '            bloom |= st0 ? m0.t : 0;', '            [branch] if (st0) mru_note(m0.t, at0);' + NL + '            bloom |= st0 ? m0.t : 0;')
        src = _rep(src, '    bloom = 0;', '    bloom = 0; mrt = 0; mrs = 0;')
        # path 6's stores are noted too
        src = _rep(src, '            bloom |= m0.t | (st1 ? m1.t : 0);', '            mru_note(m0.t, s0x); [branch] if (st1) mru_note(m1.t, at1);' + NL + '            bloom |= m0.t | (st1 ? m1.t : 0);')
        src = _rep(src, 'mop mlook(uint ad, bool look, bool mr) {', 'bool mru_has(uint t) { return ' + hit.replace('m.t', 't') + '; }' + NL + 'mop mlook(uint ad, bool look, bool mr) {')
        if P.get('s6', 0):   # x24's store pair: both texels kept (or one texel) take one branch past the lookup
            src = _rep(src, 'mop m0 = mread_entries2(t.x, i0, A0, B0, true, false), m1 = mread_entries2(t.y, i1, A1, B1, true, false);',
                       'uint ad0 = mem_addr(t.x, i0, A0, B0), ad1 = mem_addr(t.y, i1, A1, B1);' + NL
                       + '            bool fast = mru_has(ad0 >> 4) && ((ad0 >> 4) == (ad1 >> 4) || mru_has(ad1 >> 4));' + NL
                       + '            mop m0 = mlook(ad0, !fast, fast), m1 = mlook(ad1, !fast, fast);')
            src = _rep(src, """            m1.A = (s0x >> 2) == m1.b1 ? put(m1.A, s0x & 3, tg0) : m1.A;
            m1.B = (s0x >> 2) == m1.b2 ? put(m1.B, s0x & 3, tg0) : m1.B;
            mfind(m1);
            m1.sl = over ? WCS : m1.sl; m1.otag = over ? tg0 : m1.otag; m1.e = over ? (has(tg0, m1.t, m1.i) ? WCS : WCN) : m1.e;""",
                       """            [branch] if (fast) {   // (op 1's tag as op 0's store left it)
                m1.otag = same ? tg0 : m1.otag; m1.e = has(m1.otag, m1.t, m1.i) ? m1.sl : WCN;
            } else {
                m1.A = (s0x >> 2) == m1.b1 ? put(m1.A, s0x & 3, tg0) : m1.A;
                m1.B = (s0x >> 2) == m1.b2 ? put(m1.B, s0x & 3, tg0) : m1.B;
                mfind(m1);
                m1.sl = over ? WCS : m1.sl; m1.otag = over ? tg0 : m1.otag; m1.e = over ? (has(tg0, m1.t, m1.i) ? WCS : WCN) : m1.e;
            }""")
    if P.get('tfirst', 0):
        src = _rep(src, """            mop m0 = mread_entries(t.x, i0, A0, B0, st0);
            [branch] if ((((t.x ^ op_bit(8)) & op_bit(11)) | (m0.ad & 3)) != 0) m0.q = data_texel(m0.ad);""",
                   """            uint4 q0 = data_texel(mem_addr(t.x, i0, A0, B0));   // (the texel first)
            mop m0 = mread_entries(t.x, i0, A0, B0, st0);
            m0.q = q0;""")
        src = _rep(src, """            mop m0 = mread_load(t.x, i0, A0, B0), m1 = mread_load(t.y, i1, A1, B1);""",
                   """            uint4 q0 = data_texel(mem_addr(t.x, i0, A0, B0)), q1 = data_texel(mem_addr(t.y, i1, A1, B1));   // (the texels first)
            mop m0 = mread_entries(t.x, i0, A0, B0, false), m1 = mread_entries(t.y, i1, A1, B1, false);
            m0.q = q0; m1.q = q1;""")
    if P.get('nc', 0):   # GVM_W_NC: a load of read-only data looks at no tags, and is a word
        src = _rep(src, 'uint ext_k(uint w) { return (w >> 9) & 24; }', 'uint ext_k(uint w) { return ((w >> 12) & 7) == GVM_W_NC ? 0 : (w >> 9) & 24; }')
        a = 'mop mread_entries(uint w, uint imm, uint a, uint b, bool st) {'
        if 'mop mread_entries2(' in src: a = 'mop mread_entries2(uint w, uint imm, uint a, uint b, bool st, bool mr) {'
        src = _rep(src, a, a + NL + '    bool nc = ((w >> 12) & 7) == GVM_W_NC && !st;')
        i = src.index(a); j = src.index('return mlook', i); k = src.index(';', j)
        call = src[j:k]   # return mlook(ad, LOOK) or return mlook2(ad, mr, LOOK)
        p = call.rindex(')')
        c0 = call.index(', ') + 2
        if 'mlook2' in call: c0 = call.index(', ', c0) + 2
        src = src[:j] + call[:c0] + '!nc && (' + call[c0:p] + '))' + src[k:]
    if P.get('dtx', 'both') == 'one':   # a data texel from the ROM or RAM behind a branch, not both fetched and selected
        src = _rep(src, '''    uint4 rom = _Rom.Load(at), ram = _Ram.Load(at);
    return (ad & GVM_ROM) != 0 ? rom : ram;''', '''    [branch] if ((ad & GVM_ROM) != 0) return _Rom.Load(at);
    return _Ram.Load(at);''')
    R = P.get('rc', 0)
    if R:   # a read cache: the last R texels the fast paths fetched, kept in registers (RAM does not change in a pass)
        d = 'static uint rct0, rct1; static uint4 rcq0, rcq1;   // (texel + 1, 0: none) and its words' + NL
        d += 'uint4 rc_texel(uint ad) {' + NL + '    uint k = (ad >> 4) + 1;' + NL
        d += '    [branch] if (rct0 == k) return rcq0;' + NL
        if R > 1: d += '    [branch] if (rct1 == k) return rcq1;' + NL
        d += '    uint4 q = data_texel(ad);' + NL
        if R > 1: d += '    rct1 = rct0; rcq1 = rcq0;' + NL
        d += '    rct0 = k; rcq0 = q;' + NL + '    return q;' + NL + '}' + NL
        a = 'uint pick(uint4 q, uint i)'
        src = _rep(src, a, d + a)
        i = src.index('mop mread_load(uint w'); j = src.index('uint step12(')
        mid = src[i:j]
        assert 'data_texel(' in mid
        src = src[:i] + mid.replace('data_texel(', 'rc_texel(') + src[j:]
        src = _rep(src, '    bloom = 0;', '    bloom = 0; rct0 = 0; rct1 = 0;')
    if P.get('thin', 0) or P.get('thin8', 0):   # one op a bundle: the fast loop's op 1 is nothing
        i = src.index('void fstep(inout uint budget) {'); j = src.index(NL + '}' + NL, i)
        f = src[i:j]
        f = _rep(f, '    uint4 t = cur;' + NL, '    uint4 t = cur;' + NL + '    t.y = 0;   // (THIN)' + NL)
        f = _rep(f, 'A1 = r[ra1], B1 = r[rb1]', 'A1 = 0, B1 = 0')
        f = _rep(f, '    r[s1] = v1;' + NL, '')
        src = src[:i] + f + src[j:]
    if P.get('thin8', 0):   # 8-byte instructions {op, control} (16 with a long immediate), gvm_machine.thin: pc in bytes
        src = _rep(src, 'uint4 data_texel(uint ad) {', '''uint4 fetch_ins(uint p) {   // the instruction at p as a bundle {op, 0, imm, control}
    uint4 q = ram(p >> 4);
    uint2 h = (p & 8) != 0 ? q.zw : q.xy;
    return uint4(h.x, 0, q.z, h.y);
}
uint4 data_texel(uint ad) {''')
        src = _rep(src, 'uint tg = tp + asuint((int)(ctl << 6) >> 23), tn = tp + 1;', 'uint tg = tp + asuint((int)(ctl << 6) >> 23) * 8, tn = tp + ((ctl & 0x20) != 0 ? 16 : 8);')
        src = _rep(src, 'uint4 nx = ram(tn), tc = ram(tg);', 'uint4 nx = fetch_ins(tn), tc = fetch_ins(tg);')
        src = _rep(src, '        tp = pc >> 4;', '        tp = pc;')
        src = _rep(src, 'pc = b0 != budget ? tp << 4 : pc;', 'pc = b0 != budget ? tp : pc;')
        src = _rep(src, 'uint base = pc & ~15u, s = (pc >> 2) & 3, e = NONE, res = 0;', 'uint base = pc & ~7u, s = (pc & 4) != 0 ? 3u : 0u, e = NONE, res = 0;')
        src = _rep(src, 'uint4 t = ram(base >> 4);', 'uint4 t = fetch_ins(base);')
        src = _rep(src, 'pc = ((pc >> 2) & 3) >= n ? base | 12 : pc;', 'pc = (pc & 7) != 0 ? base | 4 : pc;')
        src = _rep(src, '    uint ctl = t.w, n = GVM_N(ctl);' + NL, '    uint ctl = t.w, n = GVM_N(ctl), isz = (ctl & 0x20) != 0 ? 16u : 8u;' + NL)
        src = _rep(src, 'uint base = pc & ~15u, ctl = t.w, n = GVM_N(ctl), path = ctl & 7;', 'uint base = pc & ~7u, ctl = t.w, n = GVM_N(ctl), path = ctl & 7, isz = (ctl & 0x20) != 0 ? 16u : 8u;')
        for a, b in (('ram(pc >> 4)', 'fetch_ins(pc)'), ('(pc & 15)', '(pc & 7)'), ('base | 12', 'base | 4'), ('r[rs] & ~15u', 'r[rs] & ~7u'),
                     ('asuint(GVM_OFF18(ctl)) * 16', 'asuint(GVM_OFF18(ctl)) * 8'), ('asuint(GVM_OFF9(ctl)) * 16', 'asuint(GVM_OFF9(ctl)) * 8'),
                     ('base + 16', 'base + isz'), ('pc = tvec & ~15u;', 'pc = tvec & ~7u;')):
            assert a in src, a
            src = src.replace(a, b)
    W3 = P.get('wide3', 0)
    if W3:   # three ops a bundle: n 3, op 2 (lite) in word 2 (gvm_machine.wide3); 1: behind a branch, 2: always made, into the sink unless n is 3
        src = _rep(src, '[branch] if (n == 3) { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return NONE; }', '[branch] if (n == 3 && WIDE3 == 0) { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return NONE; }')
        src = _rep(src, '        uint w = s != 0 ? t.y : t.x;' + NL + '        bool L = ((ctl >> (5 + s)) & 1) != 0;',
                   '        uint w = s == 0 ? t.x : s == 1 ? t.y : t.z;' + NL + '        bool L = s < 2 && ((ctl >> (5 + s)) & 1) != 0;')
        i = src.index('void fstep(inout uint budget) {')
        a = '    r[s1] = v1;' + NL
        j = src.index(a, i)
        if W3 == 1:
            op2 = '''    [branch] if (n8 == 0x18) {   // op 2 (WIDE3)
        uint w2 = t.z, d2 = (w2 >> 6) & 63;
        r[d2 != 0 ? d2 : 64] = alu_lite(w2, simm12(w2), r[w2 & 63], r[w2 >> 26]);
    }
'''
        else:
            op2 = '''    uint w2 = t.z, d2 = (w2 >> 6) & 63;   // op 2 (WIDE3), always made: into the sink unless the bundle has three ops
    r[n8 == 0x18 && d2 != 0 ? d2 : 64] = alu_lite(w2, simm12(w2), r[w2 & 63], r[w2 >> 26]);
'''
        src = src[:j + len(a)] + op2 + src[j + len(a):]
    if P.get('late', 0):   # the fast loop's branch on the registers as the ops left them (op 1's result wins)
        i = src.index('void fstep(inout uint budget) {')
        a = '    uint i6 = asuint((int)ctl >> 26), sum = CA + i6;'
        j = src.index(a, i)
        src = src[:j] + '    uint sl0 = path == GVM_PATH_MEM2 ? 64 : s0;   // (path 6: op 0 a store)' + NL + '    CA = ca == s1 ? v1 : ca == sl0 ? v0 : CA; CB = cb == s1 ? v1 : cb == sl0 ? v0 : CB;   // (LATEBR)' + NL + src[j:]
    return src
