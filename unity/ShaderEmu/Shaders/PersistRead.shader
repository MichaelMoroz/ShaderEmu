Shader "ShaderEmu/PersistRead"
{
    // Blit target for EmuPersist's readback (docs/system/persist.md): what the guest left in the
    // fetch's memory at 0x816c0000, one output pixel per 32-bit word, its bytes in r, g, b, a,
    // 256 words a row. Drawn only when the guest hands its kept folder over.
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

            static const uint DataRow = 64 + 0x16c000 / 2048;

            // Blit's rectangle over the whole target: clip space as the card has it, row 0 at y = 1.
            float4 vert(appdata_img v) : SV_Position {
                return float4(v.texcoord.x * 2.0 - 1.0, 1.0 - 2.0 * v.texcoord.y, 0.5, 1.0);
            }

            float4 frag(float4 at : SV_Position) : SV_Target {
                uint n = (uint)at.y * 256 + (uint)at.x;
                uint texel = n / 4, k = n & 3;
                uint4 t = _State[uint2(texel % 2048, DataRow + texel / 2048)];
                uint v = k == 0 ? t.r : k == 1 ? t.g : k == 2 ? t.b : t.a;
                return float4(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24) / 255.0;
            }
            ENDCG
        }
    }
}
