// ---- The control word's test (cc's bits, isa.h): a comparison (less, signed by bit 2), equality, a bit, or addbnz's
// sum (0, the inverse its sense); against imm6 when bit 3
uint taken(uint ctl, uint CA, uint CB, uint i6, uint sum) {   // (i6 the rb field signed, sum CA + i6)
    bool im = (ctl & 0x400) != 0;
    uint Bc = im ? i6 : CB, F = (ctl << 22) & 0x80000000u;
    bool lt = (CA ^ F) < (Bc ^ F);
    uint o = im ? (CA >> Bc) & 1 : (sum == 0 ? 1u : 0u);
    uint c = (ctl & 0x100) ? (lt ? 1u : 0u) : (ctl & 0x200) ? (CA == Bc ? 1u : 0u) : o;
    return (ctl & 0x80) ? (c == 0 ? 1u : 0u) : c;
}
