Shader "ShaderEmu/hud"
{
    // A seated frame's flat draws as the holodeck's HUD layer draws them (unity/ShaderEmu/Shaders/GpuHolodeckHudPass.cginc,
    // docs/world/holodeck.md, "The HUD layer"), for a picture without Unity: rvc_harness --hud-capture draws this pass four
    // times over a plain background, _VolumePass 0 to 3 with the HUD's blending of each (none, alpha, additive, multiply),
    // into a target where the volume's picture lies in the top left at its own pixels, as the GPU's own does.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
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
            Texture2D<float4> _Data_ROM;
            #define GPU_ROM
            uniform uint _VolumePass;   // which of the four blendings this draw is (FRAGMENT_PASS & 3)
            #define GPU_STATE _State
            #define GPU_VOLUME
            #define GPU_VOLUME_PASS _VolumePass
            #include "src/gpu_draw.h"

            // A triangle of the volume's list when it is a flat draw of this pass (volume_flat_command, as the HUD's
            // geometry shader asks), else a point nothing covers.
            gpu_varyings vert(uint id : SV_VertexID) {
                uint first = id - id % 3, base;
                uint4 head;
                if (!volume_flat_command(first, base, head)) {
                    gpu_varyings none = (gpu_varyings)0;
                    none.position = float4(-2.0, -2.0, 0.0, 1.0);
                    return none;
                }
                return gpu_vertex_sized(id, base, head, volume_picture());
            }
            float4 frag(gpu_varyings i) : SV_Target {
                return gpu_fragment(i);
            }
            ENDCG
        }
    }
}
