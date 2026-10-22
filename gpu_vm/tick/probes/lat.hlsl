// Latencies on this card (x19's calibration): one pixel runs a dependent chain of N steps of a kind
Texture2D<uint4> _T : register(t0);
cbuffer P : register(b0) { uint _N, _Kind, _Seed, _pad; };
static uint a[65];
static uint2 w[515];
uint4 frag(float4 pos : SV_POSITION) : SV_Target {
    uint x = _Seed, k;
    [loop] for (k = 0; k < 65; k++) a[k] = (k * 7 + 1) & 63;
    [loop] for (k = 0; k < 515; k++) w[k] = uint2(k * 3 & 511, k);
    [branch] if (_Kind == 0) { [loop] for (k = 0; k < _N; k++) x = x * 1664525u + 1013904223u; }
    else [branch] if (_Kind == 1) { [loop] for (k = 0; k < _N; k++) x = (x ^ (x >> 3)) + 7u; }
    else [branch] if (_Kind == 2) { [loop] for (k = 0; k < _N; k++) x = a[x & 63] + 1; }
    else [branch] if (_Kind == 3) { [loop] for (k = 0; k < _N; k++) { a[x & 63] = x; x = a[(x * 5 + 1) & 63] + 3; } }
    else [branch] if (_Kind == 4) { [loop] for (k = 0; k < _N; k++) x = _T.Load(int3(x & 8191, 0, 0)).x; }
    else [branch] if (_Kind == 5) { [loop] for (k = 0; k < _N; k++) x = (x & 1) ? x + 3 : x - 5; }
    else [branch] if (_Kind == 6) { [loop] for (k = 0; k < _N; k++) x = w[x & 511].x + 1; }
    else [branch] if (_Kind == 7) { [loop] for (k = 0; k < _N; k++) x = _T.Load(int3(x & 8191, (x >> 13) & 7, 0)).x; }
    else [branch] if (_Kind == 8) { [loop] for (k = 0; k < _N; k++) x = x + 1; }
    else [branch] if (_Kind == 9) {   // 16 independent chains of 2 ops: the issue rate
        uint y0 = x, y1 = x + 1, y2 = x + 2, y3 = x + 3, y4 = x + 4, y5 = x + 5, y6 = x + 6, y7 = x + 7;
        uint z0 = x * 3, z1 = x * 5, z2 = x * 7, z3 = x * 9, z4 = x * 11, z5 = x * 13, z6 = x * 15, z7 = x * 17;
        [loop] for (k = 0; k < _N; k++) {
            y0 = (y0 ^ 5) + 1; y1 = (y1 ^ 6) + 2; y2 = (y2 ^ 7) + 3; y3 = (y3 ^ 8) + 4; y4 = (y4 ^ 9) + 5; y5 = (y5 ^ 10) + 6; y6 = (y6 ^ 11) + 7; y7 = (y7 ^ 12) + 8;
            z0 = (z0 ^ 13) + 9; z1 = (z1 ^ 14) + 10; z2 = (z2 ^ 15) + 11; z3 = (z3 ^ 16) + 12; z4 = (z4 ^ 17) + 13; z5 = (z5 ^ 18) + 14; z6 = (z6 ^ 19) + 15; z7 = (z7 ^ 20) + 16;
        }
        x = y0 ^ y1 ^ y2 ^ y3 ^ y4 ^ y5 ^ y6 ^ y7 ^ z0 ^ z1 ^ z2 ^ z3 ^ z4 ^ z5 ^ z6 ^ z7;
    } else {   // 16 independent movc
        uint y0 = x, y1 = x + 1, y2 = x + 2, y3 = x + 3, y4 = x + 4, y5 = x + 5, y6 = x + 6, y7 = x + 7;
        [loop] for (k = 0; k < _N; k++) {
            y0 = (y0 & 1) ? y0 + 3 : y0 - 5; y1 = (y1 & 2) ? y1 + 3 : y1 - 5; y2 = (y2 & 4) ? y2 + 3 : y2 - 5; y3 = (y3 & 8) ? y3 + 3 : y3 - 5;
            y4 = (y4 & 16) ? y4 + 3 : y4 - 5; y5 = (y5 & 32) ? y5 + 3 : y5 - 5; y6 = (y6 & 64) ? y6 + 3 : y6 - 5; y7 = (y7 & 128) ? y7 + 3 : y7 - 5;
        }
        x = y0 ^ y1 ^ y2 ^ y3 ^ y4 ^ y5 ^ y6 ^ y7;
    }
    return uint4(x, 0, 0, 0);
}
float4 vert(uint id : SV_VertexID) : SV_POSITION { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0, 1); }
