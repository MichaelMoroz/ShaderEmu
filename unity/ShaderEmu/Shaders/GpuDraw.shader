Shader "ShaderEmu/GpuDraw"
{
    // The machine's GPU (docs/gpu.md): a mesh of points, one per triangle, drawn by a camera of
    // its own into the GPU's colour and depth target. One pass per GPU pass, in order.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Data_MTD_R ("ROM, first word of each texel", 2D) = "black" {}
        _Data_MTD_G ("ROM, second word", 2D) = "black" {}
        _Data_MTD_B ("ROM, third word", 2D) = "black" {}
        _Data_MTD_A ("ROM, fourth word", 2D) = "black" {}
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
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
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #define _GpuPass 7
            #include "GpuDrawPass.cginc"
            ENDCG
        }
    }
}
