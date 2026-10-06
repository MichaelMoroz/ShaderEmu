Shader "ShaderEmu/MemHeat"
{
    // Custom Render Texture behind the memory view: per pixel, a hash of the RAM texels it
    // covers (r), how recently they changed (g) and their colour packed in 24 bits (b).
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Strips ("RAM is cut into this many strips side by side", Int) = 2
    }
    SubShader
    {
        Cull Off
        ZTest Off
        Lighting Off
        Blend One Zero

        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex CustomRenderTextureVertexShader
            #pragma fragment frag
            #include "UnityCustomRenderTexture.cginc"

            Texture2D<uint4> _State;
            uint _Strips;

            static const uint TexWidth = 2048, StateRows = 64, RamRows = 4032;

            uint hash(uint x) {
                x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
                return x;
            }
            uint peak(uint v) {
                return max(max(v & 0xff, (v >> 8) & 0xff), max((v >> 16) & 0xff, v >> 24));
            }

            float4 frag(v2f_customrendertexture i) : SV_Target {
                float2 uv = i.globalTexcoord.xy;
                float2 dim = _CustomRenderTextureInfo.xy;
                uint strips = max(_Strips, 1);
                float rowsPerStrip = (float)RamRows / strips;
                float sx = uv.x * strips;
                uint strip = min((uint)sx, strips - 1);
                float2 texel = float2(frac(sx) * TexWidth, StateRows + (strip + uv.y) * rowsPerStrip);
                float2 footprint = float2(TexWidth * strips / dim.x, rowsPerStrip / dim.y);

                uint h = 0;
                uint4 first = 0;
                for (uint k = 0; k < 16; k++) {
                    float2 cell = (float2(k & 3, k >> 2) + 0.5) / 4.0;
                    int2 t = clamp(int2(texel + (cell - 0.5) * footprint), int2(0, StateRows), int2(TexWidth - 1, StateRows + RamRows - 1));
                    uint4 a = _State.Load(int3(t, 0));
                    h = hash(h ^ a.r) + hash(a.g + 0x9e3779b9u) + hash(a.b ^ 0x85ebca6bu) + hash(a.a + k);
                    if (k == 5) first = a;
                }
                float code = (float)(h & 0x7fffff);

                float4 prev = tex2D(_SelfTexture2D, uv);
                float2 px = 1.0 / dim;
                float around = max(max(tex2D(_SelfTexture2D, uv + float2(px.x, 0)).g, tex2D(_SelfTexture2D, uv - float2(px.x, 0)).g),
                                   max(tex2D(_SelfTexture2D, uv + float2(0, px.y)).g, tex2D(_SelfTexture2D, uv - float2(0, px.y)).g));
                float heat = max(prev.g * 0.955, around * 0.5);
                if (prev.r != code) heat = 1.0;

                uint3 colour = any(first != 0) ? uint3(peak(first.r), peak(first.g), peak(first.b | first.a)) : 0;
                return float4(code, heat, (float)(colour.r | colour.g << 8 | colour.b << 16), any(first != 0) ? 1 : 0);
            }
            ENDCG
        }
    }
}
