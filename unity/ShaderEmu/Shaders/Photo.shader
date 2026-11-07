Shader "ShaderEmu/Photo"
{
    // A file of the machine drawn over the whole picture of VRChat's camera (docs/apps/export.md, "A photo"): while
    // the camera (_VRChatCameraMode 1 or 2, the handheld camera in VR or on a desktop; 3, a screenshot) is inside this
    // renderer's cube, never in the player's own view (mode 0) nor in a mirror. Each pixel is three bytes of the
    // 24-bit bitmap Paint saves the picture as: the pixel (x, row from the bottom) holds the file's bytes from
    // 54 + (row * width + x) * 3, blue, green and red. The file is a zip: its entries from byte 54, its directory at
    // the file's very end, and the end's word that says where the directory begins is made here from the picture's
    // size; between them zeros. The guest's bytes are in RAM (the state texture), from texel _At (column, row), the
    // entries' and the directory's lengths in _Lengths (each as high and low 16 bits). harness/apps/rvc_persist.h
    // (photoByte) makes the same file on rvc_cpu.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _At ("The bytes' first texel: column, row", Vector) = (0, 0, 0, 0)
        _Lengths ("Entries high, low; directory high, low", Vector) = (0, 0, 0, 0)
        _Always ("Every camera (a test's)", Float) = 0
    }
    SubShader
    {
        Tags { "Queue" = "Overlay+1000" "RenderType" = "Overlay" "IgnoreProjector" = "True" }
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
            float4 _At, _Lengths;
            float _Always;
            float _VRChatCameraMode;   // (VRChat's: 0 the player's view, 1 and 2 the handheld camera, 3 a screenshot)
            float _VRChatMirrorMode;

            // The submesh's three vertices made one triangle over the whole picture, or nothing (outside clip space).
            float4 vert(uint id : SV_VertexID) : SV_Position
            {
                float3 cam = mul(unity_WorldToObject, float4(_WorldSpaceCameraPos, 1)).xyz;
                bool inside = all(abs(cam) <= 0.5);
                bool on = _Always > 0.5 || (_VRChatCameraMode > 0.5 && _VRChatMirrorMode < 0.5 && inside);
                uint k = id & 3;   // (the triangle's indices are 24 to 26: their two low bits, 0, 1, 2)
                float2 p = float2((k << 1) & 2, k & 2);
                return on ? float4(p * 2 - 1, 0.5, 1) : float4(2, 2, 2, 1);
            }

            uint ramByte(uint d)
            {
                uint texel = (uint)_At.y * 2048 + (uint)_At.x + d / 16;
                uint4 t = _State[uint2(texel % 2048, texel / 2048)];
                uint k = (d >> 2) & 3;
                uint v = k == 0 ? t.r : k == 1 ? t.g : k == 2 ? t.b : t.a;
                return (v >> ((d & 3) * 8)) & 0xff;
            }

            // The file's byte f (f >= 54).
            uint fileByte(uint f, uint fileSize, uint entries, uint tail)
            {
                uint tailAt = fileSize - tail;
                if (f - 54 < entries) return ramByte(f - 54);
                if (f < tailAt) return 0;
                uint t = f - tailAt;
                if (t >= tail - 6 && t < tail - 2) return (tailAt >> (8 * (t - (tail - 6)))) & 0xff;
                return ramByte(entries + t);
            }

            float channel(uint b)
            {
                float v = b / 255.0;
            #if !defined(UNITY_COLORSPACE_GAMMA)
                v = GammaToLinearSpaceExact(v);   // (the picture's target is sRGB: this comes out as the byte)
            #endif
                return v;
            }

            float4 frag(float4 at : SV_Position) : SV_Target
            {
                uint w = (uint)_ScreenParams.x, h = (uint)_ScreenParams.y, x = (uint)at.x, y = (uint)at.y;
                uint entries = (uint)_Lengths.x * 65536 + (uint)_Lengths.y, tail = (uint)_Lengths.z * 65536 + (uint)_Lengths.w;
                uint fileSize = 54 + w * 3 * h;
                // (a target Unity draws upside down, as a texture is: its first row is the picture's bottom)
                uint row = _ProjectionParams.x < 0 ? y : h - 1 - y;
                if ((w * 3) % 4 != 0 || 54 + entries + tail > fileSize || tail < 22)
                    return float4(1, 0, 0, 1);   // (a camera size whose rows Paint pads, or too small a picture: red)
                uint f = 54 + (row * w + x) * 3;
                return float4(channel(fileByte(f + 2, fileSize, entries, tail)), channel(fileByte(f + 1, fileSize, entries, tail)),
                              channel(fileByte(f, fileSize, entries, tail)), 1);
            }
            ENDCG
        }
    }
}
