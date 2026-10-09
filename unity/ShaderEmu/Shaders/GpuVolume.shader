Shader "ShaderEmu/GpuVolume"
{
    // The volume display (docs/volume.md): a flat screen that is a window onto the guest's 3D
    // frame. The screen is a plane of the guest camera's view, the near plane unless _Plane
    // moves it out, with the picture's rectangle there fitted to it; the scene behind it is
    // drawn as the visitor's own eyes see it, wherever VolumeMask.shader has marked the stencil.
    // One pass for each way the GPU blends, in the GPU's order.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _ScreenSize ("Width and height of the screen", Vector) = (1.28, 0.8, 0, 0)
        _Plane ("Where the screen is between the near plane (0) and the far plane (1)", Range(0, 1)) = 0
    }
    SubShader
    {
        Tags { "Queue"="Transparent-498" "RenderType"="Opaque" }   // after the mask, and after the sky
        Cull Off      // the guest's own culling is not known
        ZTest LEqual
        Stencil { Ref 87 Comp Equal }

        Pass
        {
            Name "VOLUME0"
            Blend One Zero
            ZWrite On
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 0
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "VOLUME1"
            Blend SrcAlpha OneMinusSrcAlpha, Zero One
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 1
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "VOLUME2"
            Blend SrcAlpha One, Zero One
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 2
            #include "GpuVolumePass.cginc"
            ENDCG
        }
        Pass
        {
            Name "VOLUME3"
            Blend DstColor Zero, Zero One
            ZWrite Off
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #define _VolumePass 3
            #include "GpuVolumePass.cginc"
            ENDCG
        }
    }
}
