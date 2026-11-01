// zap.h: the host's hand in the machine's RAM (docs/world/danger.md). For one commit the host may
// flip bits of one word, or of a square of texels with a different pattern in each: what a
// stray particle does to a memory chip, and what the world's "Danger zone" panel does on
// purpose. The commit includes this, the tick does not: it costs an instruction nothing.
#ifndef ZAP_H
#define ZAP_H

uniform uint _ZapKind;                 // 0 nothing, 1 one word of one texel, 2 a square of texels
uniform uint _ZapX, _ZapY;             // the texel: its column (0 to 2047) and its row of RAM's rows
uniform uint _ZapR;                    // kind 2: the square reaches this many texels each way
uniform uint _ZapWord;                 // kind 1: which of the texel's four words
uniform uint _ZapMaskLo, _ZapMaskHi;   // the bits that may flip, in two halves (a host's number may be a float)
uniform uint _ZapSeed;                 // kind 2: what the pattern is made from
uniform uint _ZapZones;                // a bit a zone (zap_zone): only those are touched

// Whose a row of RAM is, as the memory screen colours it (a row is 32 KB; MemView.shader's owner()):
// 2 the windows' buffers, 3 the display and the GPU's words, 4 a 3D program's GPU memory, 5 the sound
// card's (the devices, layout.h), then 0 the kernel and 1 Linux's programs.
uint zap_zone(uint row) {
    uint megabyte = row >> 5;
    return megabyte < 16 ? 2 : megabyte < 23 ? 3 : megabyte < 27 ? 4 : megabyte < 32 ? 5 : megabyte < 41 ? 0 : 1;
}

// The 4 MB bands of RAM a zap of this commit is in, a bit each.
uint zap_bands() {
    if (_ZapKind == 0) return 0;
    uint reach = _ZapKind == 2 ? _ZapR : 0;
    return ram_bands((_ZapY - min(_ZapY, reach)) >> 7, min((_ZapY + reach) >> 7, (STATE_TEX_HEIGHT - 65) >> 7));
}

uint zap_hash(uint n) {
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    n ^= n >> 16;
    return n;
}

// A texel of RAM (pos is its place in the state texture: RAM's rows begin at 64) as the zap leaves it.
uint4 zap(uint2 pos, uint4 texel) {
    uint row = pos.y - 64, reach = _ZapKind == 2 ? _ZapR : 0;
    if (_ZapKind == 0 || pos.x + reach < _ZapX || pos.x > _ZapX + reach || row + reach < _ZapY || row > _ZapY + reach) return texel;
    if (((_ZapZones >> zap_zone(row)) & 1) == 0) return texel;
    uint mask = _ZapMaskLo | _ZapMaskHi << 16;
    if (_ZapKind == 1)
        return texel ^ uint4(_ZapWord == 0 ? mask : 0, _ZapWord == 1 ? mask : 0, _ZapWord == 2 ? mask : 0, _ZapWord == 3 ? mask : 0);
    uint seed = zap_hash(pos.x + pos.y * 2048u + _ZapSeed * 0x9e3779b9u);
    return texel ^ (uint4(zap_hash(seed), zap_hash(seed + 1), zap_hash(seed + 2), zap_hash(seed + 3)) & mask);
}
#endif
