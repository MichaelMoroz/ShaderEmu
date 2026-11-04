Shader "ShaderEmu/GpuHolodeckHud"
{
    // The holodeck's HUD layer (docs/world/holodeck.md, "The HUD layer"): what the program draws flat over its picture
    // (a status bar, a cross-hair, messages, the console), which the world round the seat cannot place, on a quad before
    // the sitter that EmuHolodeck keeps there. Over everything of the room: no depth. One pass for each way the GPU
    // blends, in the GPU's order; its passes without depth (4-7) are drawn with the one that blends alike.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _GpuVram ("The GPU's VRAM: textures the guest uploaded, with mip levels", 2D) = "black" {}
        _HudFlip ("1: the picture's top at the quad's top; -1 turned over", Float) = 1
    }
    SubShader
    {
        Tags { "Queue"="Transparent+10" "RenderType"="Transparent" "IgnoreProjector"="True" }
        Cull Off
        ZTest Always
        ZWrite Off

        Pass
        {
            Name "HUD0"
            Blend One Zero
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 0
            #include "GpuHolodeckHudPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "HUD1"
            Blend SrcAlpha OneMinusSrcAlpha
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 1
            #include "GpuHolodeckHudPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "HUD2"
            Blend SrcAlpha One
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 2
            #include "GpuHolodeckHudPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "HUD3"
            Blend DstColor Zero
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 3
            #include "GpuHolodeckHudPass.cginc"
            ENDCG
        }
    }
}
