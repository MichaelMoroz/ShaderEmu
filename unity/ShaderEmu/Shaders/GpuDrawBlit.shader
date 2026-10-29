Shader "ShaderEmu/GpuDrawBlit"
{
    // GpuDraw.shader as blits, with no camera (docs/machine/gpu.md, "Blits in place of cameras"; src/amp.cginc): the
    // same 8 passes, each a blit into the GPU's target and its depth buffer. A blit's two triangles are two patches
    // tessellated at factor 8, and a geometry shader of 32 instances gives 5,184 calls, 13 of the device's triangles
    // each in the mesh's order (gpu_amp_geom in src/gpu.cginc). Pass 8 clears the depth to far, as the camera's
    // clear did; EmuMachine blits it first.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _StateRow ("The state texture's row RAM starts in", Float) = 64
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _AmpRank ("Each patch point's place in the card's order (BlitOrder.shader)", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" "Queue"="Geometry" }
        Cull Off
        Lighting Off

        Pass
        {
            Name "GPU0"
            Blend One Zero
            ZTest LEqual
            ZWrite On
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 0
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU1"
            Blend SrcAlpha OneMinusSrcAlpha
            ZTest LEqual
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 1
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU2"
            Blend SrcAlpha One
            ZTest LEqual
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 2
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU3"
            Blend DstColor Zero, Zero One
            ZTest LEqual
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 3
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU4"
            Blend One Zero
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 4
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU5"
            Blend SrcAlpha OneMinusSrcAlpha
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 5
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU6"
            Blend SrcAlpha One
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 6
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU7"
            Blend DstColor Zero, Zero One
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry gpu_amp_geom
            #pragma fragment frag
            #define GPU_AMP
            #define _GpuPass 7
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass   // 8: the depth cleared to far, as the GPU camera's clear (Depth) did, before passes 0 to 7
        {
            Name "CLEAR"
            ColorMask 0
            ZTest Always
            ZWrite On
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #define _GpuPass 0
            #define GPU_CLEAR
            #include "GpuDrawPass.cginc"
            ENDCG
        }
    }
}
