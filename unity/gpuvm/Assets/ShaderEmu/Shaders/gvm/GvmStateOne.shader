Shader "ShaderEmu/GvmStateOne"
{
    // A state set from the eight-target layout into GvmTickOne's one target (pass 0), and back, a blit of pass 1 a
    // target (_GvmTarget): the machine changes layouts when its cores cross gvmTickOneMax.
    Properties
    {
        _S0 ("State 0", 2D) = "black" {}
    }
    CGINCLUDE
    #pragma target 5.0
    #include "UnityCG.cginc"
    #include "tick.cginc"
    Texture2D<uint4> _One;
    uniform uint _GvmTarget;
    float4 blit_pos(float4 v : POSITION) : SV_Position { return UnityObjectToClipPos(v); }
    ENDCG
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend Off
        Pass
        {
            Name "ToOne"
            CGPROGRAM
            #pragma vertex blit_pos
            #pragma fragment to_one
            uint4 to_one(float4 at : SV_Position) : SV_Target {
                uint2 p = uint2(at.xy);
                uint c = one_core(p.x), j = p.x - one_x(c);
                return c < 256u && j < (c == 0u ? 66u : 46u) ? state_in(j * 8u + p.y, c) : uint4(0, 0, 0, 0);
            }
            ENDCG
        }
        Pass
        {
            Name "FromOne"
            CGPROGRAM
            #pragma vertex blit_pos
            #pragma fragment from_one
            uint4 from_one(float4 at : SV_Position) : SV_Target {
                // (frag's own pixel order: core 0's rows of the first block, a worker's 8 x 6 of its 8 x 16)
                uint2 p = uint2(at.xy);
                uint rx = p.x % 8u, ry = p.y % 16u, c = (p.y / 16u) * 128u + core_at(p.x / 8u);
                uint j = c == 0u ? (p.y / C0BH) * C0BLK + rx * C0BH + p.y % C0BH : rx * 6u + ry;
                bool on = c == 0u ? p.y < 12u && j < 66u : ry < 6u && j < 46u;
                return on ? _One.Load(one_at(j * 8u + _GvmTarget, c)) : uint4(0, 0, 0, 0);
            }
            ENDCG
        }
    }
}
