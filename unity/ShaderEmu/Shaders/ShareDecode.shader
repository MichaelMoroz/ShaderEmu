Shader "ShaderEmu/ShareDecode"
{
    // Another player's display (docs/share.md), from the tiles received so far: _Store has a
    // row a tile as ShareEncode.shader lays it out, with the level the tile came at in byte
    // 508 and the stamp of the packet that brought it in 509; from row 300 a row a quarter
    // of a tile at the fine level, with "present" in byte 2 and its stamp in byte 3. Writes
    // the picture from the target's top left, _Scale texels a stream pixel, for DisplayShow,
    // and only the tiles the last packet brought.
    Properties
    {
        _Store ("Received tiles", 2D) = "black" {}
        _Stream ("Width and height of the stream", Vector) = (0, 0, 0, 0)
        _TargetSize ("Size of the target", Vector) = (2048, 1024, 0, 0)
        _Scale ("Texels a stream pixel: 2 where there is a fine level", Float) = 1
        _Stamp ("The stamp of the tiles to draw", Float) = 0
        _All ("1: draw every tile", Float) = 1
        _Off ("Colour where nothing has arrived", Color) = (0.01, 0.011, 0.014, 1)
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

            Texture2D<float4> _Store;
            float4 _Stream, _TargetSize, _Off;
            float _Scale, _Stamp, _All;

            struct v2f {
                float4 pos : SV_Position;
                float2 uv : TEXCOORD0;
            };

            v2f vert(appdata_img v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.texcoord.xy;
                return o;
            }

            uint byteAt(uint t, uint n) {
                float4 v = _Store.Load(int3(n >> 2, t, 0));
                uint k = n & 3;
                return (uint)((k == 0 ? v.r : k == 1 ? v.g : k == 2 ? v.b : v.a) * 255.0 + 0.5);
            }

            float3 colourAt(uint row, uint n) {
                uint v = byteAt(row, n) | byteAt(row, n + 1) << 8;
                return float3(v >> 11, (v >> 5) & 63, v & 31) / float3(31, 63, 31);
            }

            // One of the two colours at `colours`, by a bit of the 16 at `mask`.
            float3 masked(uint row, uint colours, uint mask, uint bit) {
                uint bits = byteAt(row, mask) | byteAt(row, mask + 1) << 8;
                return colourAt(row, colours + 2 * ((bits >> bit) & 1));
            }

            // A pixel of a quarter of a tile at the fine level: its block's kind says where
            // its colours are: in its own bytes, or in those of an earlier block.
            float3 fine(uint row, uint2 in32) {
                uint block = (in32.y / 4) * 8 + in32.x / 4, bit = (in32.y % 4) * 4 + in32.x % 4;
                uint at = 20, flatAt = 20, pairAt = 20, kind = 0;
                [loop] for (uint b = 0; b <= block; b++) {
                    kind = (byteAt(row, 4 + (b >> 2)) >> (2 * (b & 3))) & 3;
                    if (kind == 1) flatAt = at;
                    if (kind == 3) pairAt = at;
                    if (b < block) at += kind == 0 ? 0 : kind == 3 ? 6 : 2;
                }
                if (kind < 2) return colourAt(row, flatAt);
                return masked(row, pairAt, kind == 2 ? at : at + 4, bit);
            }

            float4 frag(v2f i) : SV_Target {
                float2 q = float2(i.uv.x, 1.0 - i.uv.y) * _TargetSize.xy;
                uint scale = (uint)(_Scale + 0.5);
                if (q.x >= _Stream.x * scale || q.y >= _Stream.y * scale) {
                    if (_All < 0.5) discard;
                    return _Off;
                }
                uint2 own = (uint2)q, p = own / scale;
                uint t = (p.y / 32) * 20 + p.x / 32;
                uint2 inside = p % 32;
                // the quarter of the tile the pixel is in, when that has come at the fine level
                uint row = 300 + t * 4 + (inside.y / 16) * 2 + inside.x / 16;
                bool sharp = scale == 2 && byteAt(row, 2) != 0;
                uint stamp = (uint)(_Stamp + 0.5);
                if (_All < 0.5 && byteAt(t, 509) != stamp && !(sharp && byteAt(row, 3) == stamp)) discard;
                float3 c;
                if (sharp) c = fine(row, own % 32);
                else {
                    uint level = byteAt(t, 508);
                    if (level > 2) return _Off;
                    uint size = 4u << level, n = 8u >> level;
                    uint2 b = inside / size, sub = (inside % size) >> level;
                    uint at = (level == 0 ? 0 : level == 1 ? 384 : 480) + 6 * (b.y * n + b.x);
                    c = masked(t, at, at + 4, sub.y * 4 + sub.x);
                }
#ifndef UNITY_COLORSPACE_GAMMA
                c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
