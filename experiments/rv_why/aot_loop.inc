// Translated code: a fragment's arithmetic in the shader, its memory op on one shared path after the switch (experiments/rv_why)
uint aot_find(uint p) {
    uint4 m = AOT_MAP[((p >> 2) * 0x9e3779b1u) >> (32 - AOT_BITS)];
    return m.x == p ? m.y : m.z == p ? m.w : 0;
}
void aot_run(inout uint4 c[C_N], inout uint n) {
    uint a = aot_find(pc);
    [loop] while (a != 0 && n < budget) {
        uint mi = 0, ma = 0, md = 0, nx = 0;
        AOT_CASES
        [branch] if (mi != 0) {
            uint f3 = mi & 7, off = ma & 3;
            [branch] if (mi & 16) {
                bool ok = ma != 0 && off + (1u << (f3 & 3)) <= 4 && (ma ^ 0x80000000) < RAM_MAX;
                [branch] if (ok) {
                    uint up = 24 - 8 * (f3 & 3) - ((f3 & 3) == 2 ? 8 : 0);
                    uint x = ram_word(c, ma & 0x7ffffffc) >> (off * 8) << up;
                    xr[md] = (f3 & 4) ? x >> up : asuint(asint(x) >> up);
                    pc += 4; n++; a = nx;
                } else a = 0;
            } else {
                bool ok = (ma & 0x80000000) != 0 && off + (1u << f3) <= 4;
                [branch] if (ok) {
                    ram_store(c, ma & 0x7ffffffc, md << (off * 8), (f3 == 2 ? 0xffffffff : f3 == 1 ? 0xffff : 0xff) << (off * 8));
                    pc += 4; n++; a = nx;
                } else a = 0;
            }
        } else [branch] if (a == 1) a = aot_find(pc);
    }
}
