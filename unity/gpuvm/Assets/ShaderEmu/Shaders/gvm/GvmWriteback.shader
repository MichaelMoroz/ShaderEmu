Shader "ShaderEmu/GvmWriteback"
{
    // The GPU's picture copied into RAM (SUBMIT_WRITEBACK, SUBMIT_INTO), as the control pass noted it last frame:
    // four Blits into RAM, a word of each texel a pass (gpu_vm/platform/devices.shader's GPUWriteback).
    Properties
    {
        _GvmCtl ("The control words as the frame found them", 2D) = "black" {}
        _GpuTarget ("The GPU's colour target", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend One Zero
        Pass
        {
            ColorMask R
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry geom
            #pragma fragment frag
            #define _GvmComp 0u
            #include "writeback.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask G
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry geom
            #pragma fragment frag
            #define _GvmComp 1u
            #include "writeback.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask B
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry geom
            #pragma fragment frag
            #define _GvmComp 2u
            #include "writeback.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask A
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry geom
            #pragma fragment frag
            #define _GvmComp 3u
            #include "writeback.cginc"
            ENDCG
        }
    }
}
