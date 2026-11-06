Shader "ShaderEmu/GpuHolodeck"
{
    // The holodeck (docs/world/holodeck.md): the guest's 3D frame as a world round the visitor, drawn
    // wherever HolodeckMask.shader has marked the faces of the room in use. This renderer's object
    // is at the seat's eye point: the frame's camera stands there, its horizon the room's.
    // One pass for each way the GPU blends, in the GPU's order, twice: through the room's faces
    // (the stencil's mark, depth emptied), and inside the room before whatever else is seen.
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
        _ScreenSize ("The near plane's picture at the largest scale, in metres", Vector) = (1.28, 0.8, 0, 0)
        _Plane ("Scale: that plane from the near plane (0) to the far plane (1)", Range(0, 1)) = 0
        _RoomMin ("The room's low corner, in the world", Vector) = (-12.7, 0, -0.1, 0)
        _RoomMax ("The room's high corner", Vector) = (-4.7, 4, 7.9, 0)
    }
    SubShader
    {
        Tags { "Queue"="Transparent-498" "RenderType"="Opaque" }   // after the mask, and after the sky
        Cull Off      // the guest's own culling is not known
        ZTest LEqual

        Pass
        {
            Name "VOLUME0"
            Blend One Zero
            ZWrite On
            Stencil { Ref 119 ReadMask 223 WriteMask 32 Comp Equal Pass Replace }      // through the room's own faces: as far as it goes; bit 32: the world is in front here (the HUD)
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 0
            #define HOLODECK
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "VOLUME1"
            Blend SrcAlpha OneMinusSrcAlpha, Zero One
            ZWrite Off
            Stencil { Ref 87 ReadMask 223 Comp Equal }      // through the room's own faces: as far as it goes
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 1
            #define HOLODECK
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "VOLUME2"
            Blend SrcAlpha One, Zero One
            ZWrite Off
            Stencil { Ref 87 ReadMask 223 Comp Equal }      // through the room's own faces: as far as it goes
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 2
            #define HOLODECK
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "VOLUME3"
            Blend DstColor Zero, Zero One
            ZWrite Off
            Stencil { Ref 87 ReadMask 223 Comp Equal }      // through the room's own faces: as far as it goes
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 3
            #define HOLODECK
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "INSIDE0"
            Blend One Zero
            ZWrite On
            Stencil { Ref 119 ReadMask 95 WriteMask 32 Comp NotEqual Pass Replace }   // not on the room's faces (87), nor on the room's own things (HoloGuard.shader: 87 + 128); bit 32: the world in front (the HUD)
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 0
            #define HOLODECK
            #define HOLODECK_INSIDE
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "INSIDE1"
            Blend SrcAlpha OneMinusSrcAlpha, Zero One
            ZWrite Off
            Stencil { Ref 87 ReadMask 95 Comp NotEqual }   // not on the room's faces (87), nor on the room's own things (HoloGuard.shader: 87 + 128)
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 1
            #define HOLODECK
            #define HOLODECK_INSIDE
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "INSIDE2"
            Blend SrcAlpha One, Zero One
            ZWrite Off
            Stencil { Ref 87 ReadMask 95 Comp NotEqual }   // not on the room's faces (87), nor on the room's own things (HoloGuard.shader: 87 + 128)
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 2
            #define HOLODECK
            #define HOLODECK_INSIDE
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "INSIDE3"
            Blend DstColor Zero, Zero One
            ZWrite Off
            Stencil { Ref 87 ReadMask 95 Comp NotEqual }   // not on the room's faces (87), nor on the room's own things (HoloGuard.shader: 87 + 128)
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 3
            #define HOLODECK
            #define HOLODECK_INSIDE
            #include "GpuVolumePass.cginc"
            ENDCG
        }
    }
}
