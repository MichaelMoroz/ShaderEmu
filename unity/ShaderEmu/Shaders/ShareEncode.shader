Shader "ShaderEmu/ShareEncode"
{
    // What a player sends of their display (docs/share.md), made here so that Udon only copies
    // bytes. Pass 0 draws the decoded picture at the stream's size. Pass 1 packs it: a row of
    // the target per 32x32 tile, holding the tile three times over as 2-colour 4x4 blocks.
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

        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float4 _Src;       // the display's width and height, and display pixels a stream pixel
            float4 _TexSize;   // of _MainTex

            float4 vert(appdata_img v) : SV_Position {
                return UnityObjectToClipPos(v.vertex);
            }

            // Stream pixel (x, y) goes to texel (x, y), as sRGB values: the blocks are cut there.
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

        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            Texture2D<float4> _Now, _Before;   // this capture and the last one, with mipmaps

            static const uint Cols = 20;

            float4 vert(appdata_img v) : SV_Position {
                return UnityObjectToClipPos(v.vertex);
            }

            uint pack565(float3 c) {
                uint3 v = (uint3)(saturate(c) * float3(31, 63, 31) + 0.5);
                return v.r << 11 | v.g << 5 | v.b;
            }

            // Block b of tile t at a level: 4x4 samples of that mipmap, split at their mean
            // brightness into two colours (565 each) and a bit a sample.
            void block(uint t, uint level, uint b, out uint colours, out uint mask) {
                uint n = 8u >> level;
                int2 corner = int2(t % Cols, t / Cols) * (int)(32u >> level) + int2(b % n, b / n) * 4;
                float3 px[16];
                float mean = 0;
                uint i;
                for (i = 0; i < 16; i++) {
                    px[i] = _Now.Load(int3(corner + int2(i & 3, i >> 2), level)).rgb;
                    mean += dot(px[i], float3(0.299, 0.587, 0.114));
                }
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

            // A row: texels 0-95 the tile's 64 blocks, 96-119 its 16 blocks at half size, 120-125
            // its 4 at a quarter, six bytes a block. Texel 126: r, the tile changed since the
            // last capture; g, the coarsest level that still shows it exactly (2: four flat squares).
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x, t = (uint)at.y;
                if (x >= 126) {
                    if (x == 127) return 0;
                    int2 corner = int2(t % Cols, t / Cols) * 32;
                    bool changed = false, flat8 = true, flat16 = true;
                    for (int j = 0; j < 32; j++) {
                        for (int i = 0; i < 32; i++) {
                            float3 c = _Now.Load(int3(corner + int2(i, j), 0)).rgb;
                            float3 d = abs(c - _Before.Load(int3(corner + int2(i, j), 0)).rgb);
                            if (max(d.r, max(d.g, d.b)) > 0.002) changed = true;
                            d = abs(c - _Now.Load(int3(corner + int2(i & 24, j & 24), 0)).rgb);
                            if (max(d.r, max(d.g, d.b)) > 0.002) flat8 = false;
                            d = abs(c - _Now.Load(int3(corner + int2(i & 16, j & 16), 0)).rgb);
                            if (max(d.r, max(d.g, d.b)) > 0.002) flat16 = false;
                        }
                    }
                    return float4(changed ? 1.0 : 0.0, (flat16 ? 2.0 : flat8 ? 1.0 : 0.0) / 255.0, 0, 0);
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
    }
}
