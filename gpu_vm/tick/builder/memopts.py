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
    if P.get('late', 0) == 3:   # late only in a bundle with a float op: step12 and the general step; the fast loop as it was
        src = _rep(src, '    [flatten] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL) {',
                   '    [flatten] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL && ((opc0 >> 6) == 3 || fl1)) {   // (LATEBR 3: a float bundle)')
        src = _rep(src, '    [branch] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL) {',
                   '    [branch] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL && (((t.x >> 18) & 3) == 3 || (n > 1 && ((t.y >> 18) & 3) == 3))) {')
    if P.get('late', 0) in (1, 2):   # the fast loop's branch on the registers as the ops left them (op 1's result wins)
        i = src.index('void fstep(inout uint budget) {')
        a = '    uint i6 = asuint((int)ctl >> 26), sum = CA + i6;'
        j = src.index(a, i)
        if P['late'] == 2:   # read again after the ops' writes: two indexed reads, no compares and selects
            src = src[:j] + '    CA = r[ca]; CB = r[cb];   // (LATEBR 2: as the ops left them)' + NL + src[j:]
            src = _rep(src, '''        uint ca = GVM_CRA(ctl), cb = GVM_CRB(ctl);
        CA = d1 != 0 && ca == d1 ? v1 : d0 != 0 && ca == d0 ? v0 : CA; CB = d1 != 0 && cb == d1 ? v1 : d0 != 0 && cb == d0 ? v0 : CB;''',
                       '''        CA = r[GVM_CRA(ctl)]; CB = r[GVM_CRB(ctl)];   // (LATEBR 2)''')
        else:
            src = src[:j] + '    uint sl0 = path == GVM_PATH_MEM2 ? 64 : s0;   // (path 6: op 0 a store)' + NL + '    CA = ca == s1 ? v1 : ca == sl0 ? v0 : CA; CB = cb == s1 ? v1 : cb == sl0 ? v0 : CB;   // (LATEBR)' + NL + src[j:]
    if P.get('vec', 0):   # 128-bit registers: R4[k] whole (rpk=wide), the vector ops in step12 (op 0) and the general step
        V = """// ---- Phase 3: the vector ops (isa.h GVM_X_VADD ...) on whole registers; lane 0 as a scalar result
bool fltu(uint a, uint b) { return !nan_u(a) && !nan_u(b) && asfloat(a) < asfloat(b); }
bool fleu(uint a, uint b) { return !nan_u(a) && !nan_u(b) && asfloat(a) <= asfloat(b); }
uint4 vlanes(uint opc, uint4 A, uint4 B, uint4 C, uint4 D, uint imm) {
    uint4 R = 0;
    uint k;
    [unroll] for (k = 0; k < 4; k++) {
        uint a = A[k], b = B[k], c = C[k], x;
        [branch] if (opc >= GVM_X_VFADD && opc <= GVM_X_VFMAX) {
            uint f = opc == GVM_X_VFADD ? GVM_F_ADD : opc == GVM_X_VFSUB ? GVM_F_SUB : opc == GVM_X_VFMUL ? GVM_F_MUL : opc == GVM_X_VFMA ? GVM_F_MAD : opc == GVM_X_VFMIN ? GVM_F_MIN : GVM_F_MAX;
            x = fsum(f, a, b, c);
        } else {
            x = opc == GVM_X_VADD ? a + b : opc == GVM_X_VSUB ? a - b : opc == GVM_X_VMUL ? a * b : opc == GVM_X_VAND ? (a & b) : opc == GVM_X_VOR ? (a | b) : opc == GVM_X_VXOR ? (a ^ b)
              : opc == GVM_X_VFLT ? (fltu(a, b) ? ~0u : 0) : opc == GVM_X_VFLE ? (fleu(a, b) ? ~0u : 0) : opc == GVM_X_VEQ ? (a == b ? ~0u : 0) : opc == GVM_X_VSEL ? (c != 0 ? a : b)
              : opc == GVM_X_VSPLAT ? A.x : opc == GVM_X_VSWZ ? A[(imm >> (2 * k)) & 3] : opc == GVM_X_VINS ? (k == (imm & 3) ? A.x : D[k]) : 0;
        }
        R[k] = x;
    }
    [branch] if (opc == GVM_X_VDOT) {
        uint s = fsum(GVM_F_MUL, A.x, B.x, 0);
        s = fsum(GVM_F_MAD, A.y, B.y, s); s = fsum(GVM_F_MAD, A.z, B.z, s); s = fsum(GVM_F_MAD, A.w, B.w, s);
        R = uint4(s, 0, 0, 0);
    }
    return R;
}
// a vector op as op 0 (not vld, vst): writes rd whole unless its result is a scalar (vext, vmask); lane 0 back
uint vop(uint w, uint imm, uint rd) {
    uint opc = (w >> 12) & 255;
    uint4 A = R4[w & 63], B = R4[w >> 26], C = R4[(w >> 20) & 63], D = R4[rd];
    [branch] if (opc == GVM_X_VEXT) return A[imm & 3];
    [branch] if (opc == GVM_X_VMASK) return (A.x != 0 ? 1u : 0u) | (A.y != 0 ? 2u : 0u) | (A.z != 0 ? 4u : 0u) | (A.w != 0 ? 8u : 0u);
    uint4 R = vlanes(opc, A, B, C, D, imm);
    R4[rd != 0 ? rd : 64] = R;
    return R.x;
}
"""
        src = _rep(src, 'uint fb(uint w, uint imm, bool L, uint b)', V + 'uint fb(uint w, uint imm, bool L, uint b)')
        src = _rep(src, '    } else [branch] if (opc0 == GVM_OP_I(GVM_I_SEL) || opc0 == GVM_X_LPC) v0 =',
                   '    } else [branch] if (opc0 >= GVM_X_VFIRST && opc0 <= GVM_X_VLAST) v0 = vop(t.x, i0, d0);   // (VEC)' + NL
                   + '    else [branch] if (opc0 == GVM_OP_I(GVM_I_SEL) || opc0 == GVM_X_LPC) v0 =')
        src = _rep(src, '    } else {   // extended' + NL, """    } else [branch] if (opc >= GVM_X_VFIRST && opc <= GVM_X_VLAST) {   // the vector ops (VEC), vld and vst
        wb = opc == GVM_X_VEXT || opc == GVM_X_VMASK;
        [branch] if (opc == GVM_X_VLD || opc == GVM_X_VST) {
            uint ad = a + imm;
            bool vs = opc == GVM_X_VST;
            bool okv = (ad & 15) == 0 && (vs ? store_ok(ad, 16) : readable(ad) && readable(ad + 15));
            [branch] if (!okv) { trap(vs ? GVM_CAUSE_STORE : GVM_CAUSE_LOAD, epcw, ad); res = 1; }
            else [branch] if (op_waits(ad, 16, vs)) { pc = epcw; e = E_SYNC; res = 3; }
            else [branch] if (vs) {
                [branch] if (!store_any(ad, 16, R4[rd])) { pc = epcw; e = E_FULL; res = 3; }
            } else {
                uint4 q; uint x0, x1, x2, x3;
                load_any(ad, 4, x0); load_any(ad + 4, 4, x1); load_any(ad + 8, 4, x2); load_any(ad + 12, 4, x3);
                R4[rd != 0 ? rd : 64] = uint4(x0, x1, x2, x3);
            }
        } else v = vop(w, imm, rd);
    } else {   // extended
""")
        src = _rep(src, '    r[0] = 0;' + NL, '    r[0] = 0;' + NL + '    [loop] for (k = 1; k < 64; k++) R4[k].yzw = state_in(T_VR + k, c).yzw;   // (VEC: lanes 1-3)' + NL + '    R4[0] = 0;' + NL)
        src = _rep(src, '    return t >= T_WT ? uint4(NONE, NONE, NONE, NONE) :', '    [branch] if (t >= T_VR && t < T_VR + 64) return v;   // (VEC: the registers as they were)' + NL + '    return t >= T_WT ? uint4(NONE, NONE, NONE, NONE) :')
        src = _rep(src, '    [branch] if (t >= T_Q && t < T_WC) return q[t - T_Q];', '    [branch] if (t >= T_VR && t < T_VR + 64) return R4[t - T_VR];   // (VEC)' + NL + '    [branch] if (t >= T_Q && t < T_WC) return q[t - T_Q];')
    if P.get('s12in', 0):   # step12's bundles in the fast loop's loop: no leaving and entering it around each
        src = _rep(src, '        [loop] while (budget > 0 && guard > 0 && (cur.w & 7) >= thr) { fstep(budget); guard--; }',
'''        [loop] while (budget > 0 && guard > 0 && ((cur.w & 7) >= thr || (thr == GVM_PATH_FAST && (cur.w & 7) != GVM_PATH_GENERAL))) {   // (s12in)
            [branch] if ((cur.w & 7) >= GVM_PATH_FAST) fstep(budget);
            else { pc = tp << 4; uint r = step12(budget); cycle -= budget - r; budget = r; tp = pc >> 4; }   // (its own cycle and ops)
            guard--;
        }''')
    if P.get('s12b', 0):   # step12: the integer or the float unit behind a branch, not both made and one selected
        src = _rep(src, '    else v0 = (opc0 >> 6) == 1 ? alu(t.x, i0, A0, B0) : fpu(t.x, A0, fb(t.x, i0, L0, B0), C0);',
                   '    else [branch] if ((opc0 >> 6) == 1) v0 = alu(t.x, i0, A0, B0); else v0 = fpu(t.x, A0, fb(t.x, i0, L0, B0), C0);   // (s12b)')
    if P.get('fx1', 0):   # op 1 any float function but the special ones: fpu's body (its special branch never taken there)
        src = _rep(src, 'uint v1 = fl1 ? fsum(fn(t.y),', 'uint v1 = fl1 ? fpu(t.y,')
    if P.get('s12b', 0) == 2:   # op 1's float or lite behind a branch too
        i = src.index('    uint v1 = fl1 ? f'); j = src.index(chr(10), i); line = src[i:j]
        k = line.index(' : alu_lite(t.y, i1, A1, B1);')
        src = src[:i] + '    uint v1;' + chr(10) + '    [branch] if (fl1) v1 = ' + line[len('    uint v1 = fl1 ? '):k] + '; else v1 = alu_lite(t.y, i1, A1, B1);   // (s12b 2)' + src[j:]
    if P.get('vec', 0) == 2:   # lanes 1-3 read at a register's first vector use in the pass, not at its start
        src = _rep(src, '// ---- Phase 3: the vector ops', """uint4 state_in(uint t, uint c);
static uint vl0, vl1;   // registers whose lanes 1-3 are in R4 this pass
void vtouch(uint k) {
    uint b = 1u << (k & 31);
    [branch] if (((k < 32 ? vl0 : vl1) & b) == 0) {
        R4[k].yzw = state_in(T_VR + k, core).yzw;
        [flatten] if (k < 32) vl0 |= b; else vl1 |= b;
    }
}
void vmark(uint k) { uint b = 1u << (k & 31); [flatten] if (k < 32) vl0 |= b; else vl1 |= b; }
// ---- Phase 3: the vector ops""")
        src = _rep(src, '    uint4 A = R4[w & 63], B = R4[w >> 26], C = R4[(w >> 20) & 63], D = R4[rd];',
                   '    vtouch(w & 63); vtouch(w >> 26); vtouch((w >> 20) & 63); vtouch(rd);   // (vec=2)' + chr(10) + '    uint4 A = R4[w & 63], B = R4[w >> 26], C = R4[(w >> 20) & 63], D = R4[rd];')
        src = _rep(src, '                [branch] if (!store_any(ad, 16, R4[rd]))', '                vtouch(rd);' + chr(10) + '                [branch] if (!store_any(ad, 16, R4[rd]))')
        src = _rep(src, '                R4[rd != 0 ? rd : 64] = uint4(x0, x1, x2, x3);', '                R4[rd != 0 ? rd : 64] = uint4(x0, x1, x2, x3); vmark(rd);')
        src = _rep(src, '    [loop] for (k = 1; k < 64; k++) R4[k].yzw = state_in(T_VR + k, c).yzw;   // (VEC: lanes 1-3)', '    vl0 = 1; vl1 = 0;   // (vec=2: lanes at first use)')
        src = _rep(src, '    [branch] if (t >= T_VR && t < T_VR + 64) return R4[t - T_VR];   // (VEC)',
                   '    [branch] if (t >= T_VR && t < T_VR + 64) { uint k = t - T_VR; return ((((k < 32 ? vl0 : vl1) >> (k & 31)) & 1) != 0) ? R4[k] : uint4(R4[k].x, state_in(t, core).yzw); }   // (VEC)')
    return src
