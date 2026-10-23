// ---- Traps, special registers, interrupts
uint ipend() {
    bool due = (tcmp_lo | tcmp_hi) != 0 && (_TimeHi > tcmp_hi || (_TimeHi == tcmp_hi && _TimeLo >= tcmp_lo));
    return ((flags & F_SOFT) ? 1u : 0u) | (due ? 2u : 0u) | (ext << 2);
}
void trap(uint c, uint at, uint val) {
    epc = at; cause = c; tval = val;
    st = (st & GVM_ST_S ? GVM_ST_PS : 0) | (st & GVM_ST_IE ? GVM_ST_PIE : 0) | GVM_ST_S;
    pc = tvec & ~15u;
    flags &= ~F_WAIT;
}
uint sr_read(uint n) {
    [branch] if (n == GVM_SR_RANDOM) { rnd ^= rnd << 13; rnd ^= rnd >> 17; rnd ^= rnd << 5; }
    uint4 a = uint4(st, epc, cause, tval), b = uint4(tvec, scratch, ien, ipend()), c = uint4(tcmp_lo, tcmp_hi, _TimeLo, _TimeHi);
    uint4 d = uint4(cycle, core, _Player, rnd);
    uint v = n < 4 ? pick(a, n) : n < 8 ? pick(b, n) : n < 12 ? pick(c, n) : n < 16 ? pick(d, n) : n == GVM_SR_LEFT ? left : n == GVM_SR_XR ? XR : n == GVM_SR_NPAIR ? NPAIR : opsrun + 1;   // (OPS counts the op reading it)
    return v;
}
void sr_write(uint n, uint v) {
    st = n == GVM_SR_STATUS ? (v & 7) | (st & GVM_ST_S) : st;
    epc = n == GVM_SR_EPC ? v : epc; cause = n == GVM_SR_CAUSE ? v : cause; tval = n == GVM_SR_TVAL ? v : tval;
    tvec = n == GVM_SR_TVEC ? v : tvec; scratch = n == GVM_SR_SCRATCH ? v : scratch; ien = n == GVM_SR_IEN ? v : ien;
    flags = n == GVM_SR_IPEND ? (flags & ~F_SOFT) | ((v & 1) ? F_SOFT : 0) : flags;
    tcmp_lo = n == GVM_SR_TCMP_LO ? v : tcmp_lo; tcmp_hi = n == GVM_SR_TCMP_HI ? v : tcmp_hi;
}

// ---- The operations: a descriptor's two texels, and the reference's ranges (what it writes, dn bytes at its dst;
// what it reads, sn at its src)
struct opd { uint kind, dst, src, n, ds, ss, row, rows, dn, sn; };
opd op_of(uint4 a, uint4 b) {
    opd o;
    o.kind = a.x; o.dst = a.y; o.src = a.z; o.n = a.w; o.ds = b.x; o.ss = b.y; o.row = b.z; o.rows = b.w;
    bool blit = o.kind == GVM_OP_BLIT && o.rows != 0 && o.row != 0;
    o.dn = o.kind == GVM_OP_COPY || o.kind == GVM_OP_FILL ? o.n : blit ? o.ds * (o.rows - 1) + o.row : 0;
    o.sn = o.kind == GVM_OP_COPY ? o.n : blit ? o.ss * (o.rows - 1) + o.row : 0;
    return o;
}
bool in_range(uint a, uint by, uint lo, uint n) { return n != 0 && (a - lo < n || lo - a < by); }
bool op_waits(uint a, uint by, bool store) {   // an operation in flight holds it: the pass ends before the op
    [branch] if (a >= GVM_OPS) return store && a == GVM_OPS && by == 16 && qn == QMAX;
    bool w = false;
    [loop] for (uint k = 0; k < min(qn, QMAX); k++) {
        opd o = op_of(q[2 * k], q[2 * k + 1]);
        w = w || in_range(a, by, o.dst, o.dn) || (store && in_range(a, by, o.src, o.sn));
    }
    return w;
}

// ---- The general step's memory: any address, any alignment
uint word_seen(uint a) {   // a readable word's value as this core sees it
    mop m = mlook(a, true);
    uint v = pick_ad(data_texel(a), a);
    return a >= GVM_ROM ? v : m.e != WCN ? m.w : v;
}
bool readable(uint a) { return a < _RamEnd || (a >= GVM_ROM && a < GVM_ROM_END); }
bool load_any(uint a, uint by, out uint v) {   // by 1, 2 or 4; false: a fault
    v = 0;
    [branch] if (a >= GVM_OPS) {   // in flight, refused, the latched words
        uint o = a - GVM_OPS;
        v = o == 0 ? qn : o == 4 ? refused : o < 16 ? pick(lat0, o >> 2) : pick(lat1, (o >> 2) & 3);
        return o <= 32 - by && ((a | by) & 3) == 0;
    }
    bool ok = readable(a) && readable(a + by - 1);
    [branch] if (ok) {
        uint sh = (a & 3) * 8, lo = word_seen(a & ~3u), hi = 0;
        [branch] if ((a & 3) + by > 4) hi = word_seen((a & ~3u) + 4);
        uint w = sh != 0 ? (lo >> sh) | (hi << (32 - sh)) : lo;
        v = by == 4 ? w : w & ((1u << (by * 8)) - 1);
    }
    return ok;
}
bool store_ok(uint a, uint by) {
    return a >= GVM_OPS ? a - GVM_OPS <= 32 - by && ((a | by) & 3) == 0 : a >= GVM_DEV && a < _RamEnd && a + by - 1 < _RamEnd;
}
uint word_of(uint4 d, uint k) { return k < 4 ? pick(d, k) : 0; }
bool store_any(uint a, uint by, uint4 d) {   // by bytes of d at a, into the write cache; false: no room, nothing made
    uint o = a & 3, nw = (o + by + 3) >> 2, k, sl[5], wast[5], was[5], wix[5];
    bool ok = true;
    [loop] for (k = 0; k < min(nw, 5u) && ok; k++) {
        uint wi = (a >> 2) + k, t = wi >> 2, i = wi & 3;
        mop lk = mlook(wi << 2, true);
        uint s = lk.sl;
        ok = s != NONE;
        [branch] if (ok) {
            uint tg = lk.otag;
            uint e = wd[4 * s + i];
            uint old = lk.e != WCN ? lk.w : pick(ram(t), i);
            uint lo = k == 0 ? o : 0, hi = min(4u, o + by - 4 * k);
            uint m = (hi >= 4 ? 0xffffffffu : (1u << (8 * hi)) - 1) & ~((1u << (8 * lo)) - 1);
            uint val = o != 0 ? (word_of(d, k) << (8 * o)) | (k != 0 ? word_of(d, k - 1) >> (32 - 8 * o) : 0) : word_of(d, k);
            sl[k] = s; wast[k] = tg; was[k] = e; wix[k] = 4 * s + i;
            wt[s] = stag(tg, t, i); wd[4 * s + i] = old ^ ((old ^ val) & m);
            bloom |= t;
        }
    }
    [branch] if (!ok) [loop] for (k = k - 1; k > 0; k--) { wt[sl[k - 1]] = wast[k - 1]; wd[wix[k - 1]] = was[k - 1]; }   // (undone, last first)
    return ok;
}

// ---- The general step: one op of any class at slot s. 0: done; 1: trapped; 2: the pass ends after it (counted);
// 3: the pass ends before it (not counted, pc at it)
uint ext_op(uint opc, uint a, uint b) {
    uint al = a & 0xffff, ah = a >> 16, bl = b & 0xffff, bh = b >> 16;   // the high word of a * b, by halves
    uint ll = al * bl, lh = al * bh, hl = ah * bl, mid = (ll >> 16) + (lh & 0xffff) + (hl & 0xffff);
    uint hu = ah * bh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    bool sg = opc == GVM_X_DIV || opc == GVM_X_REM, na = sg && (a >> 31) != 0, nb = sg && (b >> 31) != 0;
    uint ua = na ? 0 - a : a, ub = nb ? 0 - b : b, qt = ub == 0 ? 0xffffffffu : ua / ub, m = ub == 0 ? ua : ua - qt * ub;
    bool rem = opc == GVM_X_REM || opc == GVM_X_REMU;
    uint dv = b == 0 ? (rem ? a : 0xffffffffu) : rem ? (na ? 0 - m : m) : (na != nb ? 0 - qt : qt);
    uint mh = opc == GVM_X_MULHU ? hu : hu - ((a >> 31) ? b : 0) - (opc == GVM_X_MULH && (b >> 31) ? a : 0);
    uint bits = opc == GVM_X_CLZ ? (a == 0 ? 32u : 31u - firstbithigh(a)) : opc == GVM_X_CTZ ? (a == 0 ? 32u : firstbitlow(a))
              : opc == GVM_X_POPC ? countbits(a) : opc == GVM_X_BSWAP ? (a >> 24 | ((a >> 8) & 0xff00u) | ((a << 8) & 0xff0000u) | a << 24)
              : reversebits(a);
    return opc <= GVM_X_MULHSU ? mh : opc <= GVM_X_REMU ? dv : bits;
}
uint bfmask(uint imm) { uint len = ((imm >> 5) & 31) + 1; return len >= 32 ? 0xffffffffu : (1u << len) - 1; }
//@@FILE_ANY@@
uint mem_any(uint w, uint imm, uint a, uint b, uint epcw, inout uint e, inout bool ill) {
    uint opc = (w >> 12) & 255, mode = (opc >> 3) & 7, wd = opc & 7, rd = (w >> 6) & 63;
    [branch] if (mode >= GVM_M_FREAD) return file_any(w, imm, a, epcw, e, ill);   // the core's file (gen.py's xcode)
    [branch] if (mode <= 3) {
        uint by = wd == GVM_W_W ? 4u : wd == GVM_W_D ? 8u : wd == GVM_W_Q ? 16u : wd == GVM_W_NC ? (NCLOAD != 0 && (mode & 1) == 0 ? 4u : 0u) : (wd & 1) ? 1u : 2u;
        ill = by == 0;
        uint ad = a + (mode >= 2 ? b << ((w >> 20) & 31) : imm);
        bool store = (mode & 1) != 0;
        by = by <= 4 ? min(by, 4 - (ad & 3)) : by;   // (a byte, a half or a word stays in its word)
        [branch] if (ill) return 0;
        [branch] if (op_waits(ad, by, store)) { pc = epcw; e = E_SYNC; return 3; }
        [branch] if (!store) {
            uint4 v = 0;
            bool ok = true;
            [loop] for (uint k = 0; k < clamp(by >> 2, 1u, 4u) && ok; k++) { uint x; ok = load_any(ad + 4 * k, min(by, 4u), x); v = put(v, k, x); }
            [branch] if (!ok) { trap(GVM_CAUSE_LOAD, epcw, ad); return 1; }
            v.x = wd == GVM_W_B ? asuint(asint(v.x << 24) >> 24) : wd == GVM_W_H ? asuint(asint(v.x << 16) >> 16) : v.x;
            [loop] for (uint j = 0; j < clamp(by >> 2, 1u, 4u); j++) { uint x = (rd + j) & 63; r[x == 0 ? 64 : x] = pick(v, j); }
            return 0;
        }
        [branch] if (!store_ok(ad, by)) { trap(GVM_CAUSE_STORE, epcw, ad); return 1; }
        uint4 d = uint4(r[rd], r[(rd + 1) & 63], r[(rd + 2) & 63], r[(rd + 3) & 63]);
        [branch] if (ad >= GVM_OPS) {   // the latched words; a 16-byte store at GVM_OPS asks for the operation
            [loop] for (uint k = 0; k < min(by >> 2, 4u); k++) {
                uint i = ((ad - GVM_OPS) >> 2) + k;
                lat0 = i < 4 ? put(lat0, i, pick(d, k)) : lat0; lat1 = i >= 4 ? put(lat1, i & 3, pick(d, k)) : lat1;
            }
            [branch] if (ad == GVM_OPS && by == 16) { q[2 * qn] = lat0; q[2 * qn + 1] = lat1; qn++; thr = GVM_PATH_INT; }
            return 0;
        }
        [branch] if (!store_any(ad, by, d)) { pc = epcw; e = E_FULL; return 3; }
        return 0;
    }
    ill = !(mode == GVM_M_AMO || (mode == GVM_M_AMO2 && wd <= GVM_AMO2_MAXU));
    [branch] if (ill) return 0;
    [branch] if ((a & 3) != 0 || !store_ok(a, 4) || a >= GVM_OPS) { trap(GVM_CAUSE_STORE, epcw, a); return 1; }
    [branch] if (op_waits(a, 4, true)) { pc = epcw; e = E_SYNC; return 3; }
    uint old = word_seen(a), x = r[rd];
    bool lt = asint(old) < asint(b), ltu = old < b;
    uint nw = mode == GVM_M_AMO2 ? (wd == GVM_AMO2_MINU ? (ltu ? old : b) : (ltu ? b : old))
            : wd == GVM_AMO_SWAP ? b : wd == GVM_AMO_ADD ? old + b : wd == GVM_AMO_AND ? (old & b) : wd == GVM_AMO_OR ? (old | b)
            : wd == GVM_AMO_XOR ? (old ^ b) : wd == GVM_AMO_MIN ? (lt ? old : b) : wd == GVM_AMO_MAX ? (lt ? b : old) : (old == x ? b : old);
    [branch] if (!store_any(a, 4, uint4(nw, 0, 0, 0))) { pc = epcw; e = E_FULL; return 3; }
    r[rd == 0 ? 64 : rd] = old;
    return 0;
}
uint op_any(uint w, uint imm, bool L, uint base, uint s, inout uint e) {
    uint opc = (w >> 12) & 255, rd = (w >> 6) & 63, rc = (w >> 20) & 63, cls = opc >> 6, f = opc & 31, epcw = base | (s * 4);
    uint a = r[w & 63], b = r[w >> 26], c = r[rc], v = 0, res = 0;
    bool I = (opc & 32) != 0, ill = false, wb = true, super = (st & GVM_ST_S) != 0;
    [branch] if (cls == 1) {   // integer: the unit, then the R form's carries, shadd, sel, and the I form's bfe
        uint cin = c & 1, s1 = a + b, s2 = s1 + cin, d1 = a - b;
        uint x = f == GVM_I_SHADD ? a + (b << rc) : f == GVM_I_SEL ? (c != 0 ? a : b) : f == GVM_I_ADDCY ? s2
               : f == GVM_I_CARRY ? ((s1 < a || s2 < s1) ? 1u : 0u) : f == GVM_I_SUBBY ? d1 - cin : ((a < b || d1 < cin) ? 1u : 0u);
        uint bx = a >> (imm & 31), m = bfmask(imm), bv = bx & m, top = ~m & (0 - ((bv & ~(m >> 1)) != 0 ? 1u : 0u));
        ill = I ? f > GVM_I_COMMON_LAST && f != GVM_I_BFE : f > GVM_I_BORROW;
        v = f <= GVM_I_COMMON_LAST ? alu(w, imm, a, b) : I ? bv | ((imm & 0x400) ? top : 0) : x;
    } else [branch] if (cls == 3) {   // float
        ill = I && (f == GVM_F_MAD || f == GVM_F_MSB || f == GVM_F_NMSB);
        v = fpu(w, a, I ? (L ? imm : (w >> 20) << 20) : b, c);
    } else [branch] if (cls == 2) {
        res = mem_any(w, imm, a, b, epcw, e, ill);
        wb = false;
    } else {   // extended
        wb = opc <= GVM_X_LPC || (opc >= GVM_X_MFSR && opc != GVM_X_MTSR);
        uint m = bfmask(imm) << (imm & 31), n = imm & 0xfff;
        v = opc <= GVM_X_BREV ? ext_op(opc, a, b) : opc == GVM_X_BFI ? (r[rd] & ~m) | ((a << (imm & 31)) & m) : base + imm;
        ill = opc == GVM_X_ILL || opc == 0x0f || (opc > GVM_X_FENCE && opc < GVM_X_MFSR) || opc > GVM_X_SRCLR || (opc >= GVM_X_MFSR && n >= GVM_SR_COUNT);
        [branch] if (ill) {
        } else if (opc == GVM_X_SYSCALL) { trap(GVM_CAUSE_SYSCALL, epcw, 0); res = 1; }
        else if (opc == GVM_X_BREAK) {
            [branch] if (_Flags & 1) { pc = epcw; flags |= F_HALT; e = E_HALT; res = 3; }
            else { trap(GVM_CAUSE_BREAK, epcw, 0); res = 1; }
        } else if (opc == GVM_X_WAIT) {   // a user's: the pass's end; a supervisor's: until an interrupt is pending
            pc = epcw + 4;
            bool go = super && (ipend() & ien) != 0;
            flags |= super && !go ? F_WAIT : 0;
            e = E_WAIT; res = go ? 0 : 2;
        } else if (opc == GVM_X_SYNC) { pc = epcw + 4; flags |= F_SYNC; e = E_SYNC; res = 2; }
        else if (opc >= GVM_X_MFSR) {
            [branch] if (!super && (opc != GVM_X_MFSR || n < GVM_SR_USER_FIRST)) { trap(GVM_CAUSE_PRIV, epcw, w); res = 1; }
            else {
                v = sr_read(n);
                [branch] if (opc != GVM_X_MFSR) sr_write(n, opc == GVM_X_SRSET ? v | a : opc == GVM_X_SRCLR ? v & ~a : a);
            }
        }
    }
    [branch] if (ill) { trap(GVM_CAUSE_ILLEGAL, epcw, w); res = 1; }
    opsrun += res == 0 || res == 2 ? 1 : 0;
    r[res != 0 || !wb || rd == 0 ? 64 : rd] = v;
    return res;
}
// The bundle at pc from its word on; NONE when the core goes on, else why the pass ends
uint bundle() {
    uint base = pc & ~15u, s = (pc >> 2) & 3, e = NONE, res = 0;
    [branch] if (base >= _RamEnd) { trap(GVM_CAUSE_FETCH, pc, pc); return NONE; }
    uint4 t = ram(base >> 4);
    uint ctl = t.w, n = GVM_N(ctl);
    cycle++;
    [branch] if (n == 3) { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return NONE; }
    uint ca0 = r[GVM_CRA(ctl)], cb0 = r[GVM_CRB(ctl)], cs0 = r[GVM_CRS(ctl)];   // as this run of the bundle begins
    [loop] for (; s < n && res == 0; s++) {
        uint w = s != 0 ? t.y : t.x;
        bool L = ((ctl >> (5 + s)) & 1) != 0;
        res = op_any(w, L ? t.z : simm12(w), L, base, s, e);
    }
    [branch] if (res == 1) return NONE;
    [branch] if (res != 0) { pc = ((pc >> 2) & 3) >= n ? base | 12 : pc; return e; }
    [branch] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL) { ca0 = r[GVM_CRA(ctl)]; cb0 = r[GVM_CRB(ctl)]; }   // (late branches)
    uint cc = GVM_CC(ctl), sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), next = base + 16, i6 = asuint(GVM_CIMM6(ctl)), sum = ca0 + i6;
    bool bad = cc == GVM_CC_SPECIAL ? (sub == GVM_SUB_ILLEGAL || (sub == GVM_SUB_ADD && rs == 0)) : cc == GVM_CC_ADDBNZ && GVM_CRA(ctl) == 0;
    [branch] if (bad) { trap(GVM_CAUSE_ILLEGAL, base | 12, ctl); return NONE; }
    [branch] if (cc == GVM_CC_SPECIAL) {
        uint to_far = base + asuint(GVM_OFF18(ctl)) * 16, to = r[rs] & ~15u;
        [branch] if (sub == GVM_SUB_ERET) {
            [branch] if ((st & GVM_ST_S) == 0) { trap(GVM_CAUSE_PRIV, base | 12, ctl); return NONE; }
            next = epc;
            st = (st & GVM_ST_PIE ? GVM_ST_IE : 0) | (st & GVM_ST_PS ? GVM_ST_S : 0) | GVM_ST_PIE;
        }
        next = sub == GVM_SUB_JR || sub == GVM_SUB_CALLR ? to : sub == GVM_SUB_J || sub == GVM_SUB_CALL ? to_far : next;
        r[sub == GVM_SUB_CALLR || sub == GVM_SUB_CALL ? 1 : 64] = base + 16;
        r[sub == GVM_SUB_ADD ? rs : 64] = cs0 + asuint(GVM_CIMM12(ctl));
    } else {
        r[cc == GVM_CC_ADDBNZ ? GVM_CRA(ctl) : 64] = sum;
        next = taken(ctl, ca0, cb0, i6, sum) != 0 ? base + asuint(GVM_OFF9(ctl)) * 16 : next;
    }
    pc = next;
    return NONE;
}
// The general step: an interrupt if one is due, then a bundle; the budget left, 0 when the pass ends
uint general(uint budget) {
    [branch] if (irq_due && (pc & 15) == 0) {
        uint p = ipend() & ien;
        trap(GVM_CAUSE_IRQ | ((p & 4) ? 2u : (p & 2) ? 1u : 0u), pc, 0);
    }
    left = budget - 1;
    uint e = bundle();
    e = e == NONE && NPAIR != 0 && pn >= NPAIR ? E_FULL : e;   // the bundle that filled the pair buffer ends the pass
    irq_due = (st & GVM_ST_IE) != 0 && (ipend() & ien) != 0;
    cur = ram(pc >> 4);
    cur.w &= (pc & 15) != 0 || irq_due ? ~7u : 0xffffffffu;   // (the fast loop makes whole bundles, and takes no interrupt)
    endr = e != NONE ? e : endr;
    return e != NONE ? 0 : budget - 1;
}
