Shader "ShaderEmu/Readback"
{
    // Blit target for the script's readback: state row 0 (64 texels) and the 16 control texels
    // at RAM 0x87000000, one output pixel per 32-bit word, its bytes in r, g, b, a.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
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

            static const uint Words = 320, ControlRow = 64 + 0x700000 / 2048;

            float4 vert(appdata_img v) : SV_Position {
                return UnityObjectToClipPos(v.vertex);
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                uint n = min((uint)at.x, Words - 1);
                uint texel = n / 4, k = n & 3;
                uint4 t = texel < 64 ? _State[uint2(texel, 0)] : _State[uint2(texel - 64, ControlRow)];
                uint v = k == 0 ? t.r : k == 1 ? t.g : k == 2 ? t.b : t.a;
                return float4(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24) / 255.0;
            }
            ENDCG
        }
    }
}
