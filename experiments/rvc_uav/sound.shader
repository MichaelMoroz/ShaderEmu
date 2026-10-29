Shader "ShaderEmu/sound"
{
    // The machine's sound card (docs/machine/sound.md). SoundMix draws the host's ring of samples: 128 by
    // 128 pixels, left and right in red and green, each the mix of the guest's voices at that
    // sample. The voices' own words are moved on by GPUControl (gpu.shader).
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _SoundCursor ("The sample the ring starts at", Int) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "SoundMix"
            ZTest Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            // The ROM, for samples that are files in it.
            Texture2D<float4> _Data_ROM;   // the ROM: a 16-byte texel a pixel, read by its bits (asuint)
            uniform uint _SoundCursor;
            #define SOUND_CURSOR _SoundCursor
            #define SOUND_MIXED 1
            #define SOUND_RATE 0
            #define GPU_ROM
            #define GPU_STATE _State
            #define GPU_SOUND
            #include "src/gpu.h"

            // one triangle over the whole target
            float4 vert(uint id : SV_VertexID) : SV_Position {
                return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
            }
            float4 frag(float4 at : SV_Position) : SV_Target {
                return float4(sound_mix((uint2)at.xy), 0.0, 1.0);
            }
            ENDCG
        }
    }
}
