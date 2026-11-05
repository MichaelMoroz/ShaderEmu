Shader "ShaderEmu/GpuDrawBlit"
{
    // GpuDraw.shader as blits, with no camera (docs/machine/gpu.md, "Blits in place of cameras"; src/amp.cginc): the
    // same 12 passes, each a blit into the GPU's target and its depth buffer. A blit's two triangles are two patches
    // tessellated at factor 8, and a geometry shader of 32 instances gives 5,184 calls, 13 of the device's triangles
    // each in the mesh's order (gpu_amp_geom in src/gpu.cginc). Pass 8 clears the depth to far, as the camera's
    // clear did; EmuMachine blits it first.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _StateRow ("The state texture's row RAM starts in", Float) = 64
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _AmpRank ("Each patch point's place in the card's order (BlitOrder.shader)", 2D) = "black" {}
        _GpuVram ("The VRAM: textures the guest uploaded, with mip levels (4096 x 4096 RGBA8)", 2D) = "black" {}
        _GpuClaims ("Each mesh vertex's command and passes (pass 14 writes it, 512 x 384 RGInt)", 2D) = "black" {}
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
        Pass   // 8: the depth cleared to far, as the GPU camera's clear (Depth) did, before the passes
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
        Pass
        {
            Name "GPU8"
            Blend OneMinusDstAlpha One
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
            #define _GpuPass 8
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU9"
            Blend DstColor OneMinusDstAlpha, DstAlpha OneMinusDstAlpha
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
            #define _GpuPass 9
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU12"
            Blend OneMinusDstAlpha One
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
            #define _GpuPass 12
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU13"
            Blend DstColor OneMinusDstAlpha, DstAlpha OneMinusDstAlpha
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
            #define _GpuPass 13
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            // 13: the list's uploads (CMD_UPLOAD) into the VRAM's level 0, a blit into it before the passes, every frame a
            // list is drawn (EmuMachine.GpuDraw; it then makes the VRAM's levels when a list asked, SUBMIT_UPLOADS)
            Name "GPUUpload"
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
            #define GPU_UPLOAD_PASS
            #define _GpuPass 0
            #include "GpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            // 14: each mesh vertex's command and passes (docs/machine/gpu.md, "Claims"), a blit into EmuMachine.claims
            // before the uploads, every frame a list is drawn; the uploads and the 12 passes read it
            Name "GPUClaims"
            Blend One Zero
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #define GPU_CLAIMS_PASS
            #define _GpuPass 0
            #include "GpuDrawPass.cginc"
            ENDCG
        }
    }
}
