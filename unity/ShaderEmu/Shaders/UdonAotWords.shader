// Texels of the machine's state as words for EmuUdonCpu's readback, a word a pixel (bytes in r,
// g, b, a), the target 1024 wide: _CountA texels from _TexelA, then from _TexelB. A texel number
// is RAM's (address - 0x80000000) / 16.
Shader "ShaderEmu/UdonAotWords"
{
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _TexelA ("The first part's first texel", Float) = 0
        _CountA ("Its texels", Float) = 0
        _TexelB ("The second part's first texel", Float) = 0
    }
    SubShader
    {
        Pass
        {
            ZTest Always Cull Off ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert_img
            #pragma fragment frag
            #include "UnityCG.cginc"

            Texture2D<uint4> _State;
            float _TexelA, _CountA, _TexelB;

            float4 frag(v2f_img i) : SV_Target
            {
                uint n = (uint)i.pos.y * 1024 + (uint)i.pos.x, a = (uint)_CountA;
                uint t = n / 4 < a ? (uint)_TexelA + n / 4 : (uint)_TexelB + (n / 4 - a);
                uint4 x = _State[uint2(t % 8192, 64 + t / 8192)];
                uint k = n & 3, v = k == 0 ? x.r : k == 1 ? x.g : k == 2 ? x.b : x.a;
                return float4(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24) / 255.0;
            }
            ENDCG
        }
    }
}
