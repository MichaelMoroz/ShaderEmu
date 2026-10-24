Shader "ShaderEmu/GvmTick"
{
    // gpu_vm's tick (tick.cginc, gpu_vm/tick/STATE.md) drawn by the tick camera into the eight textures of one state
    // set, reading the other: a quad a core's block (8 x GEO_BH of 1024 x GVM_STATE_ROWS), the cores from _NCores on collapsed.
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
            Name "Tick"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex tick_vert
            #pragma fragment frag
            #include "tick.cginc"
            static const uint GVM_STATE_ROWS = max(24u, GEO_BAND + GEO_BH);   // (ShaderEmuGvm.cs: the state textures' rows)
            v2f tick_vert(uint id : SV_VertexID) {
                uint c = id / 6, v = id % 6;
                float x = (c % 128) * 8 + ((v == 1 || v == 3 || v == 4) ? 8 : 0);
                float y = (c / 128) * GEO_BAND + ((v == 2 || v == 4 || v == 5) ? GEO_BH : 0);
                v2f o;
                bool apart = GVM_HAS_CORE0 && c == 0;   // (c0nb: core 0 is GvmTick0's)
                o.vertex = c < _NCores && !apart ? float4(x / 1024.0 * 2 - 1, 1 - y / (float)GVM_STATE_ROWS * 2, 0, 1) : float4(-2, -2, 0, 1);
                return o;
            }
            ENDCG
        }
    }
}
