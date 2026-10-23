Shader "ShaderEmu/GvmTick"
{
    // gpu_vm's tick (tick.cginc, gpu_vm/tick/STATE.md) drawn by the tick camera into the eight textures of one state
    // set, reading the other: a quad a core's block (8 x GEO_BH of 1024 x 24), the cores from _NCores on collapsed.
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
            v2f tick_vert(uint id : SV_VertexID) {
                uint c = id / 6, v = id % 6;
                float x = (c % 128) * 8 + ((v == 1 || v == 3 || v == 4) ? 8 : 0);
                float y = (c / 128) * GEO_BAND + ((v == 2 || v == 4 || v == 5) ? GEO_BH : 0);
                v2f o;
                o.vertex = c < _NCores ? float4(x / 1024.0 * 2 - 1, 1 - y / 24.0 * 2, 0, 1) : float4(-2, -2, 0, 1);
                return o;
            }
            ENDCG
        }
    }
}
