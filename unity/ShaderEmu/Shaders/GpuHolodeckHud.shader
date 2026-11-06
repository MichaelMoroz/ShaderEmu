Shader "ShaderEmu/GpuHolodeckHud"
{
    // The holodeck's HUD layer (docs/world/holodeck.md, "The HUD layer"): what the program draws flat over its picture
    // (a status bar, a cross-hair, messages, the console), which the world round the seat cannot place, on a quad before
    // the sitter that EmuHolodeck keeps there. It floats over the program's world, never over what is nearer of the room:
    // where the world is in front (stencil bit 32, GpuHolodeck.shader's solid passes) it has no depth test, the world's own
    // near faces (a weapon at the eye) under it; elsewhere it is tested at its own distance, so an avatar, a hand or the room's
    // things before it hide it. Drawn after the world (its material 2503) and the hands' aims (2504), and before the room's transparent things (its panels, the beams'
    // UI), which draw over it where nearer. One pair of passes for each way the GPU blends, in the GPU's order; its passes
    // without depth (4-7) are drawn with the one that blends alike.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _GpuVram ("The GPU's VRAM: textures the guest uploaded, with mip levels", 2D) = "black" {}
        _GpuDrawIds ("Each mesh vertex's command and passes (512 x 384 RGInt)", 2D) = "black" {}
        // the vertex front end's eight (GpuFront.shader; ShaderEmuBuilder.GpuInputs): declared, or what the builder sets is not kept
        _GpuMadePos ("The vertex front end's pos (512 x 385)", 2D) = "black" {}
        _GpuMadeColour ("The vertex front end's colour (512 x 385)", 2D) = "black" {}
        _GpuMadeUv ("The vertex front end's uv (512 x 385)", 2D) = "black" {}
        _GpuMadeEdge ("The vertex front end's edge (512 x 385)", 2D) = "black" {}
        _GpuMadeTex ("The vertex front end's tex (512 x 385)", 2D) = "black" {}
        _GpuMadeMore ("The vertex front end's more (512 x 385)", 2D) = "black" {}
        _GpuMadeEye ("The vertex front end's eye (512 x 385)", 2D) = "black" {}
        _GpuMadeProj ("The vertex front end's proj (512 x 385)", 2D) = "black" {}
        _HudFlip ("1: the picture's top at the quad's top; -1 turned over", Float) = 1
    }
    SubShader
    {
        Tags { "Queue"="Transparent-495" "RenderType"="Transparent" "IgnoreProjector"="True" }
        Cull Off
        ZWrite Off

        Pass
        {
            Name "HUD0World"
            Blend One Zero
            ZTest Always
            Stencil { Ref 32 ReadMask 32 Comp Equal }   // the world in front: over it
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
            Name "HUD0Near"
            Blend One Zero
            ZTest LEqual
            Stencil { Ref 32 ReadMask 32 Comp NotEqual }   // the room or an avatar in front: at its distance
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
            Name "HUD1World"
            Blend SrcAlpha OneMinusSrcAlpha
            ZTest Always
            Stencil { Ref 32 ReadMask 32 Comp Equal }   // the world in front: over it
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
            Name "HUD1Near"
            Blend SrcAlpha OneMinusSrcAlpha
            ZTest LEqual
            Stencil { Ref 32 ReadMask 32 Comp NotEqual }   // the room or an avatar in front: at its distance
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
            Name "HUD2World"
            Blend SrcAlpha One
            ZTest Always
            Stencil { Ref 32 ReadMask 32 Comp Equal }   // the world in front: over it
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
            Name "HUD2Near"
            Blend SrcAlpha One
            ZTest LEqual
            Stencil { Ref 32 ReadMask 32 Comp NotEqual }   // the room or an avatar in front: at its distance
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
            Name "HUD3World"
            Blend DstColor Zero
            ZTest Always
            Stencil { Ref 32 ReadMask 32 Comp Equal }   // the world in front: over it
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
        Pass
        {
            Name "HUD3Near"
            Blend DstColor Zero
            ZTest LEqual
            Stencil { Ref 32 ReadMask 32 Comp NotEqual }   // the room or an avatar in front: at its distance
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
