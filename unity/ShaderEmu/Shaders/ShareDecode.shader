Shader "ShaderEmu/ShareDecode"
{
    // Another player's display (docs/share.md), from the tiles received so far: _Store has a
    // row a tile as ShareEncode.shader lays it out, with the level the tile came at in texel
    // 127. Writes the picture a pixel a texel from the target's top left, for DisplayShow.
    Properties
    {
        _Store ("Received tiles", 2D) = "black" {}
        _Stream ("Width and height of the picture", Vector) = (0, 0, 0, 0)
        _TargetSize ("Size of the target", Vector) = (1024, 512, 0, 0)
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

            float4 frag(v2f i) : SV_Target {
                float2 q = float2(i.uv.x, 1.0 - i.uv.y) * _TargetSize.xy;
                if (q.x >= _Stream.x || q.y >= _Stream.y) return _Off;
                uint2 p = (uint2)q;
                uint t = (p.y / 32) * 20 + p.x / 32;
                uint level = byteAt(t, 508);
                if (level > 2) return _Off;
                uint2 inside = p % 32;
                uint size = 4u << level, n = 8u >> level;
                uint2 b = inside / size, sub = (inside % size) >> level;
                uint at = (level == 0 ? 0 : level == 1 ? 384 : 480) + 6 * (b.y * n + b.x);
                uint mask = byteAt(t, at + 4) | byteAt(t, at + 5) << 8;
                if ((mask >> (sub.y * 4 + sub.x)) & 1) at += 2;
                uint v = byteAt(t, at) | byteAt(t, at + 1) << 8;
                float3 c = float3(v >> 11, (v >> 5) & 63, v & 31) / float3(31, 63, 31);
#ifndef UNITY_COLORSPACE_GAMMA
                c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
