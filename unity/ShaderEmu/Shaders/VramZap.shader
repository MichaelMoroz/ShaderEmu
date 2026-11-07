// The danger zone's radiation in the GPU's VRAM (docs/world/danger.md, "The VRAM"): a hit inverts
// every bit of chosen channels in a small rectangle of the VRAM's level 0, and nothing else.
// A blit draws its quad over the rectangle only (the vertex stage puts it there), and the blend does
// the flip: with Blend OneMinusDstColor OneMinusSrcColor a source of 1 gives 1 - dst, which on an
// 8-bit channel is every bit flipped exactly, and a source of 0 leaves dst as it was. The draws read
// the VRAM alone since the textures went there, so radiation in RAM no longer reached a texture.
Shader "ShaderEmu/VramZap"
{
    Properties
    {
        _ZapRect ("Rectangle (x0, y0, x1, y1), 0 to 1", Vector) = (0, 0, 0, 0)
        _ZapChannels ("Channels to flip, 0 or 1 each", Vector) = (1, 0, 0, 0)
        _ZapSeed ("Seed: which pixels of the rectangle", Float) = 0
        _ZapDensity ("Share of the rectangle's pixels hit, 0 to 1", Float) = 1
    }
    SubShader
    {
        Cull Off ZWrite Off ZTest Always
        Blend OneMinusDstColor OneMinusSrcColor
        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4 _ZapRect, _ZapChannels;
            float _ZapSeed, _ZapDensity;

            struct v2f { float4 pos : SV_POSITION; float2 place : TEXCOORD0; };

            v2f vert(appdata_img v)
            {
                v2f o;
                float2 at = lerp(_ZapRect.xy, _ZapRect.zw, v.texcoord.xy);
                o.pos = float4(at * 2 - 1, 0, 1);
                o.place = at;
                return o;
            }

            uint hash(uint n)
            {
                n ^= n >> 16; n *= 0x7feb352du; n ^= n >> 15; n *= 0x846ca68bu; n ^= n >> 16;
                return n;
            }

            float4 frag(v2f i) : SV_Target
            {
                uint2 p = (uint2)i.pos.xy;
                uint h = hash(p.x * 73856093u ^ p.y * 19349663u ^ (uint)_ZapSeed);
                if ((h & 0xffff) >= (uint)(_ZapDensity * 65536.0)) return 0;   // (0: this pixel kept)
                return _ZapChannels;
            }
            ENDCG
        }
    }
}
