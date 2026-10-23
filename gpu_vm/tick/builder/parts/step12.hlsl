// ---- Paths 1 and 2 (op 0 of the whole integer unit, sel, lpc or a float op; or a load or store; then lite or float lite),
// a whole bundle with any control word but eret: x16's full step, both ops' registers read first. A load or store is
// noted as the fast loop's. jr and callr take rs after the ops, the add from rs as the bundle found it (isa.h).
uint fb(uint w, uint imm, bool L, uint b) { return (w & op_bit(32)) ? (L ? imm : (w >> 20) << 20) : b; }
uint step12(uint budget) {   // the budget left: 0 when a store found no room (nothing made; the bundle again next pass)
    uint4 t = cur;
    uint base = pc & ~15u, ctl = t.w, n = GVM_N(ctl), path = ctl & 7;
    uint tg = base + asuint(GVM_OFF9(ctl)) * 16;
    bool L0 = (ctl & 0x20) != 0, L1 = (ctl & 0x40) != 0, ok = true;
    uint i0 = L0 ? t.z : simm12(t.x), i1 = L1 ? t.z : simm12(t.y);
    uint d0 = GVM_RD(t.x), d1 = GVM_RD(t.y), ra1 = GVM_RA(t.y), rb1 = GVM_RB(t.y);
    uint A0 = r[GVM_RA(t.x)], B0 = r[GVM_RB(t.x)], A1 = r[ra1], B1 = r[rb1], CA = r[GVM_CRA(ctl)], CB = r[GVM_CRB(ctl)], v0 = 0;
    bool fl1 = (t.y & op_bit(0xc0)) == op_bit(0xc0);   // (op 1 of class 3: a float; else lite)
    uint opc0 = (t.x >> 12) & 255, C0 = r[GVM_RC(t.x)], cs0 = r[GVM_CRS(ctl)];
    [branch] if (path == GVM_PATH_MEMF) {   // a load or a store into its word as this core sees it, then a float sum
        mop m = mread(t.x, i0, A0, B0);
        bool stv = (t.x & op_bit(8)) != 0;
        smax = max(smax, stv ? m.ad - GVM_DEV : 0); lmax = max(lmax, stv ? 0 : m.ad & ~GVM_ROM);
        uint word = seen(m), sl = slot(m);
        v0 = extend(t.x, word, m.ad);
        ok = !stv || sl != NONE;
        uint at = stv && ok ? sl : WCN;
        wt[at] = stag(slot_tag(m, sl), m.t, m.i);
        wd[4 * at + m.i] = merge(t.x, word, m.ad, r[d0]);
        bloom |= stv ? m.t : 0;
        d0 = stv ? 0 : d0;
    } else [branch] if (opc0 == GVM_OP_I(GVM_I_SEL) || opc0 == GVM_X_LPC) v0 = opc0 == GVM_X_LPC ? base + i0 : C0 != 0 ? A0 : B0;
    else v0 = (opc0 >> 6) == 1 ? alu(t.x, i0, A0, B0) : fpu(t.x, A0, fb(t.x, i0, L0, B0), C0);
    [branch] if (!ok) return 0;
    r[d0 == 0 ? 64 : d0] = v0;
    A1 = (d0 != 0 && ra1 == d0) ? v0 : A1; B1 = (d0 != 0 && rb1 == d0) ? v0 : B1;
    uint v1 = fl1 ? fsum(fn(t.y), A1, fb(t.y, i1, L1, B1), r[GVM_RC(t.y)]) : alu_lite(t.y, i1, A1, B1);
    r[d1 == 0 ? 64 : d1] = v1;
    [flatten] if (LATEBR != 0 && GVM_CC(ctl) != GVM_CC_SPECIAL) {   // (late branches: the registers as the ops left them)
        uint ca = GVM_CRA(ctl), cb = GVM_CRB(ctl);
        CA = d1 != 0 && ca == d1 ? v1 : d0 != 0 && ca == d0 ? v0 : CA; CB = d1 != 0 && cb == d1 ? v1 : d0 != 0 && cb == d0 ? v0 : CB;
    }
    uint i6 = asuint(GVM_CIMM6(ctl)), sum = CA + i6;
    r[GVM_CC(ctl) != GVM_CC_ADDBNZ ? 64 : GVM_CRA(ctl)] = sum;   // after the ops' writes: it wins
    pc = taken(ctl, CA, CB, i6, sum) != 0 ? tg : base + 16;
    [branch] if (GVM_CC(ctl) == GVM_CC_SPECIAL) {   // jr, callr (rs after the ops), j, call, the add (rs as found)
        uint sub = GVM_SUB(ctl), rs = GVM_CRS(ctl), to = r[rs] & ~15u;
        pc = sub == GVM_SUB_JR || sub == GVM_SUB_CALLR ? to : sub == GVM_SUB_J || sub == GVM_SUB_CALL ? base + asuint(GVM_OFF18(ctl)) * 16 : base + 16;
        r[sub == GVM_SUB_CALL || sub == GVM_SUB_CALLR ? 1 : 64] = base + 16;
        r[sub == GVM_SUB_ADD ? rs : 64] = cs0 + asuint(GVM_CIMM12(ctl));
    }
    cur = ram(pc >> 4);
    cycle++;
    opsrun += n;
    return budget - 1;
}
