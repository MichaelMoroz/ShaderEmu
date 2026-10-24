Shader "ShaderEmu/GvmTick"
{
    // gpu_vm's tick (tick.cginc, gpu_vm/tick/STATE.md) drawn by the tick camera into the eight textures of one state
    // set, reading the other: a quad a core's block (8 x GEO_BH of 1024 x GVM_STATE_ROWS), the cores from _NCores on collapsed.
    // A tick with idlew 4 (GVM_COPY) leaves out each worker parked as it was, and the second pass copies their state by
    // its frag_copy, a shader that runs nothing: every core is written every pass, and no parked one starts the tick.
    Properties
    {
        _Ram ("RAM", 2D) = "black" {}
        _Rom ("ROM", 2D) = "black" {}
    }
    CGINCLUDE
    #pragma target 5.0
    #include "tick.cginc"
    static const uint GVM_STATE_ROWS = max(24u, GEO_BAND + GEO_BH);   // (ShaderEmuGvm.cs: the state textures' rows)
    v2f quad_vert(uint id, bool copies) {
        uint c = id / 6, v = id % 6;
        float x = (c % 128) * 8 + ((v == 1 || v == 3 || v == 4) ? 8 : 0);
        float h = C0RT != 0 && c == 0 ? max(GEO_BH, C0ROWS) : GEO_BH;   // (c0rt: core 0's rows, in the first band, in the same draw)
        float y = (c / 128) * GEO_BAND + ((v == 2 || v == 4 || v == 5) ? h : 0);
        v2f o;
        bool apart = GVM_HAS_CORE0 && C0RT == 0 && c == 0;   // (c0nb: core 0 is GvmTick0's)
        bool parked = false;
#if GVM_COPY
        o.cc = c;
        parked = c != 0 && c < _NCores && parked_as_was(c);   // (the world has no external lines)
#endif
        bool shown = c < _NCores && !apart && parked == copies;
        o.vertex = shown ? float4(x / 1024.0 * 2 - 1, 1 - y / (float)GVM_STATE_ROWS * 2, 0, 1) : float4(-2, -2, 0, 1);
        return o;
    }
    v2f tick_vert(uint id : SV_VertexID) { return quad_vert(id, false); }
#if GVM_COPY
    v2f copy_vert(uint id : SV_VertexID) { return quad_vert(id, true); }
#else
    v2f copy_vert(uint id : SV_VertexID) { v2f o = (v2f)0; o.vertex = float4(-2, -2, 0, 1); return o; }
    tick_out frag_copy(v2f i) { tick_out o = (tick_out)0; discard; return o; }
#endif
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
            Name "Tick"
            CGPROGRAM
            #pragma vertex tick_vert
            #pragma fragment frag
            ENDCG
        }
        Pass
        {
            Name "Copy"
            CGPROGRAM
            #pragma vertex copy_vert
            #pragma fragment frag_copy
            ENDCG
        }
    }
}
