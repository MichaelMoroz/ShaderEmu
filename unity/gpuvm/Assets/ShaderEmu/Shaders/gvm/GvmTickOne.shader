Shader "ShaderEmu/GvmTickOne"
{
    // gpu_vm's tick as a blit into one target, no camera (tick.cginc's GVM_ONE): the state set is one texture 8 rows tall,
    // core c's pixel j (texels 8j to 8j + 7 of the eight-target tick) is column one_x(c) + j and each row writes one of
    // its texels. Core 0 is 72 columns, a worker 48: whole 4 x 8 and 8 x 8 tiles, drawn by one triangle over the whole
    // target (the blit's first primitive grown, its second dropped), so no edge crosses a core and no warp holds two.
    // A parked worker's block keeps its state as it was inside frag, as GvmTick's copy pass does.
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
            #pragma vertex one_vert
            #pragma geometry one_geo
            #pragma fragment frag_one
            #define GVM_ONE
            #include "tick.cginc"
            struct one_v { float4 p : TEXCOORD0; };
            struct one_g { float4 vertex : SV_POSITION; nointerpolation uint cc : CORE; float cut : SV_ClipDistance0; };
            one_v one_vert(float4 v : POSITION) { one_v o; o.p = v; return o; }
            // (the cores from _NCores on are cut off by a clip distance at their first column: a vertical cut launches
            // no pixel past it and mixes no warps, gpu_vm/tick/WAVES.md)
            [maxvertexcount(3)]
            void one_geo(triangle one_v I[3], uint pid : SV_PrimitiveID, inout TriangleStream<one_g> s) {
                [branch] if (pid != 0) return;
                float2 wh;
                _S0.GetDimensions(wh.x, wh.y);
                float right = (float)min(one_x(_NCores), (uint)wh.x) / wh.x * 2 - 1;
                one_g a;
                a.cc = NONE;
                a.vertex = float4(-1, 1, 0, 1); a.cut = right - a.vertex.x; s.Append(a);
                a.vertex = float4(3, 1, 0, 1); a.cut = right - a.vertex.x; s.Append(a);
                a.vertex = float4(-1, -3, 0, 1); a.cut = right - a.vertex.x; s.Append(a);
            }
            ENDCG
        }
    }
}
