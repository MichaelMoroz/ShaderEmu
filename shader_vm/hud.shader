Shader "ShaderEmu/hud"
{
    // A seated frame's flat draws as the holodeck's HUD layer draws them (unity/ShaderEmu/Shaders/GpuHolodeckHudPass.cginc,
    // docs/world/holodeck.md, "The HUD layer"), for a picture without Unity: rvc_harness --hud-capture draws this pass four
    // times over a plain background, _VolumePass 0 to 3 with the HUD's blending of each (none, alpha, additive, multiply),
    // into a target where the volume's picture lies in the top left at its own pixels, as the GPU's own does.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "HUDDraw"
            ZTest Off
            ZWrite Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            uniform uint _VolumePass;   // which of the four blendings this draw is (FRAGMENT_PASS & 3)
            #define GPU_STATE _State
            #define GPU_VOLUME
            #define GPU_VOLUME_PASS _VolumePass
            #include "src/gpu_draw.h"

            // A triangle of the device's last list when it is a flat draw of this pass (volume_triangle, as the HUD's
            // geometry shader asks), as the front end made it, else a point nothing covers.
            gpu_varyings vert(uint id : SV_VertexID) {
                gpu_varyings o = (gpu_varyings)0;
                o.position = float4(-2.0, -2.0, 0.0, 1.0);
                if (volume_triangle(id - id % 3, VOLUME_FLAT)) o = gpu_vertex_made(id);
                return o;
            }
            float4 frag(gpu_varyings i) : SV_Target {
                return gpu_fragment(i);
            }
            ENDCG
        }
    }
}
