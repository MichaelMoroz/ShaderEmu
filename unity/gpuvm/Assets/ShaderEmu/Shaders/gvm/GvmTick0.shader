Shader "ShaderEmu/GvmTick0"
{
    // Core 0's tick when the step gives it a write cache of its own (c0nb: WCN0, NTEX0; harness/apps/gvm_backend11.cpp,
    // "core0"): tick.cginc compiled with CORE0=1, drawn by the tick camera with GvmTick's mesh as one quad over core 0's
    // block, 8 wide and as many rows as its pixels take, at the top left of the set; GvmTick.shader leaves core 0 out.
    // With a step that has no CORE0, ShaderEmuGvm.cs turns its renderer off.
    Properties
    {
        _Ram ("RAM", 2D) = "black" {}
        _Rom ("ROM", 2D) = "black" {}
    }
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
            Name "Tick0"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex tick0_vert
            #pragma fragment frag
            #define CORE0 1
            #include "tick.cginc"
            v2f tick0_vert(uint id : SV_VertexID) {
                uint v = id % 6, rows = (SPIX + 8 * GEO_BH - 1) / (8 * GEO_BH) * GEO_BH;
                float x = (v == 1 || v == 3 || v == 4) ? 8 : 0;
                float y = (v == 2 || v == 4 || v == 5) ? rows : 0;
                v2f o;
                o.vertex = id < 6 ? float4(x / 1024.0 * 2 - 1, 1 - y / (float)max(24u, GEO_BAND + GEO_BH) * 2, 0, 1) : float4(-2, -2, 0, 1);
                return o;
            }
            ENDCG
        }
    }
}
