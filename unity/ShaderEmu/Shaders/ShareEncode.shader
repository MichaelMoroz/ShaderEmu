Shader "ShaderEmu/ShareEncode"
{
    // What a player sends of their display (docs/share.md), made here so that Udon only copies
    // bytes. Pass 0 draws the decoded picture at the stream's size, or at the display's own.
    // Pass 1 packs the stream: a row of the target per 32x32 tile, holding the tile three
    // times over as 2-colour 4x4 blocks. Passes 2 to 4 are the fine level: the display's own
    // pixels, a row a quarter of a tile, with repeated colours left out.
    Properties
    {
        _MainTex ("Pass 0: the decoded display", 2D) = "black" {}
    }
    SubShader
    {
        Cull Off
        ZTest Always
        ZWrite Off
        Blend One Zero

        CGINCLUDE
        #pragma target 5.0
        #pragma vertex vert
        #pragma fragment frag
        #include "UnityCG.cginc"

        Texture2D<float4> _Now, _Before;           // this capture and the last one, with mipmaps
        Texture2D<float4> _FineNow, _FineBefore;   // the same at the display's own size
        Texture2D<float4> _Changed;                // pass 2's: a texel a tile
        Texture2D<float4> _FineBlocks;             // pass 3's: two texels a 4x4 block
        float4 _Fine;                              // x: there is a fine level; y: every tile counts as changed

        static const uint Cols = 20, Tiles = 300;

        float4 vert(appdata_img v) : SV_Position {
            return UnityObjectToClipPos(v.vertex);
        }

        uint pack565(float3 c) {
            uint3 v = (uint3)(saturate(c) * float3(31, 63, 31) + 0.5);
            return v.r << 11 | v.g << 5 | v.b;
        }

        uint byte_of(float v) {
            return (uint)(v * 255.0 + 0.5);
        }

        // 4x4 samples split at their mean brightness into two colours (5:6:5 each: the means
        // of the darker and of the lighter) and a bit a sample.
        void split(float3 px[16], out uint colours, out uint mask) {
            float mean = 0;
            uint i;
            for (i = 0; i < 16; i++) mean += dot(px[i], float3(0.299, 0.587, 0.114));
            mean /= 16.0;
            float3 low = 0, high = 0;
            float lows = 0, highs = 0;
            mask = 0;
            for (i = 0; i < 16; i++) {
                if (dot(px[i], float3(0.299, 0.587, 0.114)) > mean + 1e-4) {
                    high += px[i];
                    highs += 1;
                    mask |= 1u << i;
                } else {
                    low += px[i];
                    lows += 1;
                }
            }
            low /= max(lows, 1.0);
            high = highs > 0 ? high / highs : low;
            colours = pack565(low) | pack565(high) << 16;
        }

        bool tile_changed(uint t) {
            return _Fine.y > 0.5 || _Changed.Load(int3(t % Cols, t / Cols, 0)).r > 0.5;
        }
        ENDCG

        // 0: picture pixel (x, y) to texel (x, y), as sRGB values: the blocks are cut there.
        Pass
        {
            CGPROGRAM
            sampler2D _MainTex;
            float4 _Src;       // the display's width and height, and display pixels a texel
            float4 _TexSize;   // of _MainTex

            float4 frag(float4 at : SV_Position) : SV_Target {
                float2 q = (floor(at.xy) + 0.5) * _Src.z;
                if (q.x >= _Src.x || q.y >= _Src.y) return 0;
                float3 c = tex2Dlod(_MainTex, float4(q.x / _TexSize.x, 1.0 - q.y / _TexSize.y, 0, log2(_Src.z))).rgb;
#ifndef UNITY_COLORSPACE_GAMMA
                c = c <= 0.0031308 ? c * 12.92 : 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
#endif
                return float4(c, 1);
            }
            ENDCG
        }

        // 1: the stream's rows (the target's first 300). Texels 0-95 the tile's 64 blocks,
        // 96-119 its 16 blocks at half size, 120-125 its 4 at a quarter, six bytes a block.
        // Texel 126: r, the tile changed since the last capture; g, the coarsest level that
        // still shows it exactly (2: four flat squares).
        Pass
        {
            CGPROGRAM
            // Block b of tile t at a level: 4x4 samples of that mipmap.
            void block(uint t, uint level, uint b, out uint colours, out uint mask) {
                uint n = 8u >> level;
                int2 corner = int2(t % Cols, t / Cols) * (int)(32u >> level) + int2(b % n, b / n) * 4;
                float3 px[16];
                for (uint i = 0; i < 16; i++) px[i] = _Now.Load(int3(corner + int2(i & 3, i >> 2), level)).rgb;
                split(px, colours, mask);
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x, t = (uint)at.y;
                if (t >= Tiles) discard;
                if (x >= 126) {
                    if (x == 127) return 0;
                    int2 corner = int2(t % Cols, t / Cols) * 32;
                    bool flat8 = true, flat16 = true;
                    for (int j = 0; j < 32; j++) {
                        for (int i = 0; i < 32; i++) {
                            float3 c = _Now.Load(int3(corner + int2(i, j), 0)).rgb;
                            float3 d = abs(c - _Now.Load(int3(corner + int2(i & 24, j & 24), 0)).rgb);
                            if (max(d.r, max(d.g, d.b)) > 0.002) flat8 = false;
                            d = abs(c - _Now.Load(int3(corner + int2(i & 16, j & 16), 0)).rgb);
                            if (max(d.r, max(d.g, d.b)) > 0.002) flat16 = false;
                        }
                    }
                    return float4(_Changed.Load(int3(t % Cols, t / Cols, 0)).r, (flat16 ? 2.0 : flat8 ? 1.0 : 0.0) / 255.0, 0, 0);
                }
                uint level = x < 96 ? 0 : x < 120 ? 1 : 2;
                uint first = (x - (level == 0 ? 0 : level == 1 ? 96 : 120)) * 4;
                uint c0, m0, c1, m1;
                block(t, level, first / 6, c0, m0);
                block(t, level, (first + 3) / 6, c1, m1);
                float4 o = 0;
                for (uint k = 0; k < 4; k++) {
                    uint n = first + k, part = n % 6;
                    bool second = n / 6 != first / 6;
                    uint c = second ? c1 : c0, m = second ? m1 : m0;
                    o[k] = (part < 4 ? (c >> (8 * part)) & 255 : (m >> (8 * (part - 4))) & 255) / 255.0;
                }
                return o;
            }
            ENDCG
        }

        // 2: a texel a tile: 1 when it differs from the capture before, in the display's own
        // pixels where there is a fine level (a change there can vanish at half size).
        Pass
        {
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                int2 tile = (int2)at.xy;
                bool fine = _Fine.x > 0.5;
                int side = fine ? 64 : 32;
                bool changed = false;
                [loop] for (int j = 0; j < side; j++) {
                    [loop] for (int i = 0; i < side; i++) {
                        int3 p = int3(tile * side + int2(i, j), 0);
                        float3 d;
                        if (fine) d = abs(_FineNow.Load(p).rgb - _FineBefore.Load(p).rgb);
                        else d = abs(_Now.Load(p).rgb - _Before.Load(p).rgb);
                        if (max(d.r, max(d.g, d.b)) > 0.002) changed = true;
                    }
                }
                return changed ? 1.0 : 0.0;
            }
            ENDCG
        }

        // 3: the fine level's blocks, of the tiles that changed: 4x4 of the display's own
        // pixels each, two texels a block: its two colours, then its bits.
        Pass
        {
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint2 p = (uint2)at.xy;
                uint2 b = uint2(p.x / 2, p.y);
                if (!tile_changed((b.y / 16) * Cols + b.x / 16)) discard;
                float3 px[16];
                for (uint i = 0; i < 16; i++) px[i] = _FineNow.Load(int3(b * 4 + uint2(i & 3, i >> 2), 0)).rgb;
                uint colours, mask;
                split(px, colours, mask);
                if ((p.x & 1) == 0) return float4(colours & 255, (colours >> 8) & 255, (colours >> 16) & 255, colours >> 24) / 255.0;
                return float4(mask & 255, mask >> 8, 0, 0) / 255.0;
            }
            ENDCG
        }

        // 4: the fine level's rows (from the target's row 300), four a tile: a quarter of the
        // tile, 32x32 of the display's pixels as 64 blocks. Two bytes of length, two unused,
        // 16 bytes of two bits a block, then each block's bytes: 0, of one colour, the one
        // of the last such block (none); 1, of one colour (2); 2, of the two colours of the
        // last block that had two (2: its bits); 3, of two colours (6).
        Pass
        {
            CGPROGRAM
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x, y = (uint)at.y;
                if (y < Tiles) discard;
                uint t = (y - Tiles) / 4, part = (y - Tiles) % 4;
                if (t >= Tiles || !tile_changed(t)) discard;
                uint2 corner = uint2(t % Cols, t / Cols) * 16 + uint2(part & 1, part >> 1) * 8;
                uint flat = 0x10000, pair = 0xffffffff, where = 0, k;
                uint4 o = 0;
                [loop] for (uint b = 0; b < 64; b++) {
                    uint2 bp = corner + uint2(b & 7, b >> 3);
                    float4 cv = _FineBlocks.Load(int3(bp.x * 2, bp.y, 0)), mv = _FineBlocks.Load(int3(bp.x * 2 + 1, bp.y, 0));
                    uint low = byte_of(cv.r) | byte_of(cv.g) << 8, high = byte_of(cv.b) | byte_of(cv.a) << 8;
                    uint mask = byte_of(mv.r) | byte_of(mv.g) << 8;
                    uint kind, size, data0 = 0, data1 = 0;   // the block's bytes: up to four, then two
                    if (low == high || mask == 0) {
                        kind = low == flat ? 0 : 1;
                        size = kind == 0 ? 0 : 2;
                        flat = low;
                        data0 = low;
                    } else {
                        uint both = low | high << 16;
                        kind = both == pair ? 2 : 3;
                        size = kind == 2 ? 2 : 6;
                        pair = both;
                        data0 = kind == 2 ? mask : both;
                        data1 = mask;
                    }
                    for (k = 0; k < 4; k++) {
                        uint n = x * 4 + k;
                        if (n >= 4 && n < 20 && (b >> 2) == n - 4) o[k] |= kind << (2 * (b & 3));
                        if (n >= 20 && n - 20 >= where && n - 20 < where + size) {
                            uint i = n - 20 - where;
                            o[k] = i < 4 ? (data0 >> (8 * i)) & 255 : (data1 >> (8 * (i - 4))) & 255;
                        }
                    }
                    where += size;
                }
                if (x == 0) {
                    o[0] = (16 + where) & 255;
                    o[1] = (16 + where) >> 8;
                }
                return (float4)o / 255.0;
            }
            ENDCG
        }
    }
}
