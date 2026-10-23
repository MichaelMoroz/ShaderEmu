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
    return src
