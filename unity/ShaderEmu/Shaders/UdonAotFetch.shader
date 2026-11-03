// A picture VRChat fetched for EmuUdonCpu's guest, as the 256 KB of words a fetch's answer is
// (gpu.h's fetch_word): its pixels from _Offset, or with _Packed the bytes of a file it carries.
Shader "ShaderEmu/UdonAotFetch"
{
    Properties
    {
        _MainTex ("The picture", 2D) = "white" {}
        _Offset ("The part's first byte", Float) = 0
        _Packed ("1: a file in the pixels' red, green and blue", Float) = 0
    }
    SubShader
    {
        Pass
        {
            ZTest Always Cull Off ZWrite Off
            CGPROGRAM
            #pragma vertex vert_img
            #pragma fragment frag
            #pragma target 5.0
            #include "UnityCG.cginc"

            Texture2D<float4> _MainTex;
            float _Offset, _Packed;

            float3 seen(float4 c)   // the texture is sRGB
            {
                return c.rgb <= 0.0031308 ? c.rgb * 12.92 : 1.055 * pow(abs(c.rgb), 1.0 / 2.4) - 0.055;
            }

            uint3 packed_pixel(uint p, uint2 size)
            {
                float4 c = _MainTex.Load(int3(p % size.x, size.y - 1 - min(p / size.x, size.y - 1), 0));
                return (uint3)(saturate(seen(c)) * 255.0 + 0.5);
            }

            float4 frag(v2f_img i) : SV_Target
            {
                uint2 size;
                uint levels;
                _MainTex.GetDimensions(0, size.x, size.y, levels);
                uint n = (uint)i.pos.y * 256 + (uint)i.pos.x, word;
                if (_Packed > 0.5)
                {
                    uint at = (uint)_Offset + 4 * n, p = at / 3, k = at % 3;
                    uint3 a = packed_pixel(p, size), b = packed_pixel(p + 1, size);
                    uint six[6] = {a.r, a.g, a.b, b.r, b.g, b.b};
                    word = six[k] | (six[k + 1] << 8) | (six[k + 2] << 16) | (six[k + 3] << 24);
                }
                else
                {
                    n += (uint)_Offset / 4;
                    if (n >= size.x * size.y) return 0;
                    float4 c = _MainTex.Load(int3(n % size.x, size.y - 1 - n / size.x, 0));
                    uint4 b = (uint4)(saturate(float4(seen(c), c.a)) * 255.0 + 0.5);   // red, green, blue, alpha (fetch.md)
                    word = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
                }
                return float4(word & 255, (word >> 8) & 255, (word >> 16) & 255, word >> 24) / 255.0;
            }
            ENDCG
        }
    }
}
