Shader "ShaderEmu/Sound"
{
    // The sound card's mix (experiments/rvc_opt/sound.shader's, docs/machine/sound.md), drawn by
    // EmuSound.cs with Blit into a 128 x 128 two-float target: the ring of samples from the
    // cursor on, left and right, already at the volume the visitor chose.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _SoundCursorLo ("The sample the ring starts at, low half", Int) = 0
        _SoundCursorHi ("and high half", Int) = 0
        _Volume ("Volume", Float) = 0.1
        _StateRow ("Row of the state texture RAM starts in (EmuUdonCpu: its gathered copy)", Float) = 64
    }
    SubShader
    {
        Cull Off
        ZTest Always
        ZWrite Off
        Blend One Zero

        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            Texture2D<uint4> _State;
            Texture2D<float4> _Data_ROM;   // the ROM: a 16-byte texel a pixel, read by its bits (asuint)
            uniform uint _SoundCursorLo, _SoundCursorHi;
            uniform float _Volume, _StateRow;
            #define SOUND_CURSOR (_SoundCursorLo | (_SoundCursorHi << 16))
            #define SOUND_MIXED 1
            #define SOUND_RATE 0
            #define GPU_ROM
            #define GPU_STATE _State
            #define GPU_STATE_ROW ((uint)(int)_StateRow)
            #define GPU_SOUND
            #include "src/gpu.cginc"

            float4 vert(appdata_img v) : SV_Position {
                return UnityObjectToClipPos(v.vertex);
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                return float4(sound_mix((uint2)at.xy) * _Volume, 0.0, 1.0);
            }
            ENDCG
        }
    }
}
