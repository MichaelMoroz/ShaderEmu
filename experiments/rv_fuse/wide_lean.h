// wide.h's loop for experiments/rvc_lean (the world's tick): the same bundles (make with xlate_snap.py --sink 64: f0 is xr[32]
// there), its names: pc, clock, budget, ram_word, ram_store. An old step's word leaves to the lean fast loop.
#define WIDE_B0 (2304u * 2048u)
#define WIDE_PCT (5000u * 2048u)
#define CODE_TEX(c) asuint(_Data_ROM[uint2((c) & 2047, (c) >> 11)])

uint wide_lookup(uint p) {
    uint k = p - 0x82000000u, q = p - 0x84000000u;
    uint at = k < 0x800000u ? k >> 4 : (256u * 2048u) + (q >> 4);
    bool inr = k < 0x800000u || q < 0x4000000u;
    return inr ? CODE_TEX(at)[(p >> 2) & 3] : 0u;
}

uint wide_run(inout uint4 c[C_N], uint room) {
    uint b = wide_lookup(pc);
    if (b == 0) return 0;
    budget = room;
    uint trips = 0, rv = 0;
    uint4 cur = CODE_TEX(b), nx = CODE_TEX(b + 1);
    [loop]
    while (true) {
        uint ctl = cur.w, X = cur.x, path = ctl & 7;
        uint a = xr[(X >> 6) & 0x1f], y = xr[(X >> 11) & 0x1f];
        uint v = 0, dst = X & 0x7f;
        [branch]
        if (path < 2) {
            uint bv = y + cur.z;
            [branch]
            if (path == 0) {
                v = a + bv;
            } else {
                uint f = (X >> 16) & 15;
                uint sh = bv & 0x1f;
                uint r0 = (f & 1) ? a - bv : a + bv;
                uint r1 = (f & 1) ? a | bv : a & bv;
                uint r2 = (f & 1) ? a << sh : a ^ bv;
                uint r3 = (f & 1) ? asuint(asint(a) >> sh) : a >> sh;
                uint turn = (f & 1) ? 0 : 0x80000000;
                uint r4 = (a ^ turn) < (bv ^ turn) ? 1 : 0;
                v = (f & 8) ? r4 : (f & 4) ? ((f & 2) ? r3 : r2) : ((f & 2) ? r1 : r0);
            }
        } else {
            [branch]
            if (path >= 4) break;
            uint va = a + cur.z;
            uint f3 = (X >> 16) & 7, off = va & 3;
            [branch]
            if (path == 2) {
                [branch]
                if (!(va != 0 && off + (1u << (f3 & 3)) <= 4 && (va ^ 0x80000000) < RAM_MAX)) break;
                uint m = ram_word(c, va & 0x7ffffffc) >> (off * 8);
                uint up = (f3 & 3) == 0 ? 24 : (f3 & 3) == 1 ? 16 : 0;
                m <<= up;
                v = (f3 & 4) ? m >> up : asuint(asint(m) >> up);
            } else {
                [branch]
                if (!((va & 0x80000000) != 0 && off + (1u << f3) <= 4)) break;
                ram_store(c, va & 0x7ffffffc, y << (off * 8), (f3 == 2 ? 0xffffffff : f3 == 1 ? 0xffff : 0xff) << (off * 8));
            }
        }
        xr[dst] = v;
        uint Y = cur.y;
        xr[Y & 0x7f] = xr[(Y >> 6) & 0x1f] + xr[(Y >> 11) & 0x1f] + asuint(asint(Y) >> 20);
        rv += (ctl >> 3) & 7;
        trips++;
        [branch]
        if ((ctl & 0xc0) != 0) {
            uint ca = xr[(ctl >> 11) & 0x1f];
            uint kind = ctl & 0xc0;
            bool go = true;
            uint next = b;
            [branch]
            if (kind == 0x40) {
                uint cb = xr[(ctl >> 16) & 0x1f];
                uint f = (ctl >> 8) & 7;
                bool lt = (f & 2) ? ca < cb : asint(ca) < asint(cb);
                go = ((f & 4) ? lt : ca == cb) != ((f & 1) != 0);
                next = b + asuint(asint(ctl) >> 21);
            } else if (kind == 0x80) {
                next = b + asuint(asint(ctl) >> 8);
            } else {
                pc = (ca + asuint(asint(ctl) >> 21)) & 0xfffffffe;
                next = wide_lookup(pc);
                [branch]
                if (next == 0) { b = 0; break; }
            }
            [branch]
            if (go) {
                b = next;
                [branch]
                if (trips >= budget) break;
                cur = CODE_TEX(b);
                nx = CODE_TEX(b + 1);
                continue;
            }
        }
        b++;
        [branch]
        if (trips >= budget) break;
        cur = nx;
        nx = CODE_TEX(b + 1);
    }
    [branch]
    if (b != 0) pc = CODE_TEX(WIDE_PCT + ((b - WIDE_B0) >> 2))[(b - WIDE_B0) & 3];
    clock += rv;
    return trips;
}
