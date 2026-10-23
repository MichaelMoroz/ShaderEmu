uint op_bit(uint b) { return b << 12; }   // an opc bit in an op word
uint simm12(uint w) { return asuint((int)w >> 20); }
uint fn(uint w) { return (w >> 12) & 31; }

// ---- Integers: every result in parallel, then a tree of selects on f's bits (isa.h's numbering; DXBC's shifts take
// the low 5 bits, so a rotate's 32 - s is -y). The full unit (0-23), the mid one (0-19, the fast loop's op 0), lite
// (0-7, 16-18: op 1).
uint alu_g0(uint w, uint x, uint y) {   // 0-7
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0;
    uint f2v = (w >> 14) & 1, yi = y ^ (0 - f2v), sum = x + yi + f2v;   // (sub: x + ~y + 1)
    return f1 ? (f0 ? x ^ yi : x | yi) : (f0 ? x & yi : sum);
}
uint alu_g1(uint w, uint x, uint y) {   // 8-15: slt sltu min minu sge sgeu max maxu
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0, f2 = (w & op_bit(4)) != 0;
    uint F = f0 ? 0u : 0x80000000u;
    bool lt = (x ^ F) < (y ^ F);
    bool c = f2 ? !lt : lt;
    return f1 ? (c ? x : y) : (c ? 1u : 0u);
}
uint alu_lo(uint w, uint x, uint y) {   // 16-19: sll srl sra mul
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0;
    return f1 ? (f0 ? x * y : asuint(asint(x) >> y)) : (f0 ? x >> y : x << y);
}
uint alu(uint w, uint imm, uint a, uint b) {
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0, f2 = (w & op_bit(4)) != 0;
    uint x = a, y = (w & op_bit(32)) ? imm : b, ny = 0 - y;
    uint hi = f1 ? ((x == y ? 1u : 0u) ^ (f0 ? 1u : 0u)) : (f0 ? (x << y) | (x >> ny) : (x >> y) | (x << ny));   // 20-23: ror rol seq sne
    uint g2 = f2 ? hi : alu_lo(w, x, y);
    return (w & op_bit(16)) ? g2 : (w & op_bit(8)) ? alu_g1(w, x, y) : alu_g0(w, x, y);
}
uint alu_mid(uint w, uint imm, uint a, uint b) {
    uint x = a, y = (w & op_bit(32)) ? imm : b;
    return (w & op_bit(16)) ? alu_lo(w, x, y) : (w & op_bit(8)) ? alu_g1(w, x, y) : alu_g0(w, x, y);
}
uint alu_lite(uint w, uint imm, uint a, uint b) {
    uint x = a, y = (w & op_bit(32)) ? imm : b;
    bool f0 = (w & op_bit(1)) != 0, f1 = (w & op_bit(2)) != 0;
    return (w & op_bit(16)) ? (f1 ? asuint(asint(x) >> y) : (f0 ? x >> y : x << y)) : alu_g0(w, x, y);
}

// ---- Floats: the I form's operand is its imm12 as a float's top bits (or the long immediate)
uint fnan(float v) { uint u = asuint(v); return (u & 0x7fffffffu) > 0x7f800000u ? 0x7fc00000u : u; }
bool nan_u(uint u) { return (u & 0x7fffffffu) > 0x7f800000u; }
uint fsum(uint f, uint ra, uint rb, uint rc) {   // f 0-7: add mul mad msb sub nmsb min max
    precise float a = asfloat(ra);
    precise float b = asfloat(rb);
    precise float c = asfloat(rc);
    precise float p = a * b;
    precise float pr = asfloat(fnan(p));   // the product rounded before the sum
    bool prod = f == GVM_F_MAD || f == GVM_F_MSB, nm = f == GVM_F_NMSB;
    precise float X = prod ? pr : nm ? c : a;
    precise float Y = prod ? c : nm ? pr : b;
    precise float s = (f == GVM_F_MSB || f == GVM_F_SUB || nm) ? X - Y : X + Y;
    bool an = nan_u(ra), bn = nan_u(rb);
    bool less = a < b || (a == b && (ra >> 31) > (rb >> 31)), more = a > b || (a == b && (rb >> 31) > (ra >> 31));
    uint mm = an ? (bn ? 0x7fc00000u : rb) : bn ? ra : (((f & 1) != 0 ? more : less) ? ra : rb);
    return f >= GVM_F_MIN ? mm : f == GVM_F_MUL ? fnan(p) : fnan(s);
}
uint itof(uint a) { bool n = (a >> 31) != 0; return asuint((float)(n ? 0 - a : a)) | (n ? 0x80000000u : 0); }   // (fxc2: by size and sign)
uint fpu(uint w, uint ra, uint rb, uint rc) {
    uint f = fn(w), v;
    [branch] if (f >= GVM_F_DIV) {   // the special functions, behind a branch
        precise float a = asfloat(ra);
        precise float y;
        [branch] if (f == GVM_F_DIV) {   // (the card divides by 1 / b: two ordinary numbers with their exponents moved)
            uint ea = (ra >> 23) & 0xff, eb = (rb >> 23) & 0xff;
            int de = (int)ea - (int)eb, ha = clamp(de, -126, 127);
            precise float da = asfloat((ra & 0x807fffffu) | ((uint)(127 + ha) << 23));
            precise float db = asfloat((rb & 0x807fffffu) | ((uint)(127 - (de - ha)) << 23));
            y = (ea - 1 < 254 && eb - 1 < 254) ? da / db : a / asfloat(rb);
        } else y = f == GVM_F_SQRT ? sqrt(a) : f == GVM_F_RSQRT ? rsqrt(a) : f == GVM_F_RCP ? rcp(a) : f == GVM_F_SIN ? sin(a)
                 : f == GVM_F_COS ? cos(a) : f == GVM_F_EXP2 ? exp2(a) : log2(a);
        v = fnan(y);
    } else {
        precise float a = asfloat(ra);
        precise float b = asfloat(rb);
        bool an = nan_u(ra), bn = nan_u(rb), f0 = (f & 1) != 0, f1 = (f & 2) != 0;
        // f 8-15: eq lt le unord sgnj sgnjn sgnjx frac
        bool cmp = !an && !bn && (f == GVM_F_EQ ? a == b : f == GVM_F_LT ? a < b : a <= b);
        uint sg = (ra & 0x7fffffffu) | ((f == GVM_F_SGNJ ? rb : f == GVM_F_SGNJN ? ~rb : ra ^ rb) & 0x80000000u);
        uint g1 = (f & 4) ? (f == GVM_F_FRAC ? fnan(a - floor(a)) : sg) : f == GVM_F_UNORD ? ((an || bn) ? 1u : 0u) : (cmp ? 1u : 0u);
        // f 16-23: itof utof ftoi ftou floor ceil trunc round
        precise float wh = f1 ? (f0 ? round(a) : trunc(a)) : (f0 ? ceil(a) : floor(a));
        uint toi = an ? 0 : a >= 2147483648.0 ? 0x7fffffffu : a <= -2147483648.0 ? 0x80000000u : asuint((int)a);
        uint tou = (an || a <= 0.0) ? 0 : a >= 4294967296.0 ? 0xffffffffu : a >= 2147483648.0 ? 0x80000000u + (uint)(a - 2147483648.0) : (uint)a;
        uint g2 = (f & 4) ? fnan(wh) : f1 ? (f0 ? tou : toi) : (f0 ? asuint((float)ra) : itof(ra));
        v = (f & 16) ? g2 : (f & 8) ? g1 : fsum(f, ra, rb, rc);
    }
    return v;
}
