Shader "ShaderEmu/Display"
{
    // The machine's display (docs/display.md), decoded from the state texture: modes 1-4 and
    // the cursor. With _Raw it writes the picture one pixel a texel from the target's top left
    // corner (EmuMachine draws that for DisplayShow.shader); without, it fits it into a quad.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _GpuTarget ("The GPU's colour target", 2D) = "black" {}
        _Aspect ("Width / height of the quad", Float) = 1.7777778
        _Off ("Colour while the display is off", Color) = (0.01, 0.011, 0.014, 1)
        _Brightness ("Brightness", Float) = 1
        [ToggleUI] _Power ("Machine is on", Float) = 1
        [ToggleUI] _Raw ("One pixel a texel", Float) = 0
        _RawSize ("Size of the target with _Raw", Vector) = (2048, 1024, 0, 0)
        _HostPointer ("The host's pointer in display pixels (z: it is on the display)", Vector) = (0, 0, 0, 0)
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }

        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            Texture2D<uint4> _State;
            Texture2D<float4> _GpuTarget;
            float _Aspect, _Brightness, _Power, _Raw;
            float4 _RawSize, _HostPointer;
            float4 _Off;

            static const uint DispCtrl = 0x700000, DispPalette = 0x700040, DispPixels = 0x700100;
            static const uint DispCursor = 0x700004;   // x, y, on, address of a 32x32 image

            struct v2f {
                float4 pos : SV_Position;
                float2 uv : TEXCOORD0;
            };

            v2f vert(appdata_base v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.texcoord.xy;
                return o;
            }

            uint4 ram(uint index) {
                return _State[uint2(index % 2048, 64 + index / 2048)];
            }
            uint word(uint4 t, uint i) {
                return i == 0 ? t.r : i == 1 ? t.g : i == 2 ? t.b : t.a;
            }
            uint ram_word(uint address) {
                return word(ram((address & 0x7fffffff) >> 4), (address >> 2) & 3);
            }
            float3 rgb(uint v) {
                return float3((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff) / 255.0;
            }

            // The picture's colour at pixel q (already inside it), without the cursor.
            float3 picture(float2 q, uint4 ctrl) {
                uint mode = ctrl.r, w = ctrl.g;
                int2 at = int2(floor(q));
                if (mode == 4) {
                    // layers: a table of rectangles of RAM, the last one on top
                    uint table = (ctrl.a & 0x7fffffff) >> 4, count = min(ram(table).r, 16u);
                    for (uint n = count; n > 0; n--) {
                        uint4 box = ram(table + 2 * n - 1);
                        int2 d = at - int2(asint(box.r), asint(box.g));
                        if (d.x >= 0 && d.y >= 0 && d.x < (int)box.b && d.y < (int)box.a)
                            return rgb(ram_word(ram(table + 2 * n).r + 4 * (uint)(d.y * (int)box.b + d.x)));
                    }
                    return 0;
                }
                if (mode == 3) return _GpuTarget.Load(int3(at, 0)).rgb;
                uint i = (uint)at.y * w + (uint)at.x;
                if (mode == 1) return rgb(word(ram(DispPixels + i / 4), i & 3));
                uint b = (word(ram(DispPixels + i / 16), (i / 4) & 3) >> (8 * (i & 3))) & 0xff;
                return rgb(word(ram(DispPalette + b / 4), b & 3));
            }

            float4 frag(v2f i) : SV_Target {
                float2 spread = fwidth(i.uv);   // before any branch
                uint4 ctrl = ram(DispCtrl);
                uint mode = ctrl.r, w = ctrl.g, h = ctrl.b;
                if (_Power < 0.5 || mode < 1 || mode > 4 || w == 0 || h == 0 || w > 2048 || h > 2048) return _Off;
                float2 size = float2(w, h);
                float shape = size.x / size.y;
                // centred, as large as fits
                float2 p = float2(i.uv.x, 1.0 - i.uv.y) - 0.5;
                float2 fit = float2(max(1.0, _Aspect / shape), max(1.0, shape / _Aspect));
                p *= fit;
                float2 q = (p + 0.5) * size;
                if (_Raw > 0.5) q = float2(i.uv.x, 1.0 - i.uv.y) * _RawSize.xy;
                if (q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) return _Off;

                float3 c = 0;
                bool done = false;
                uint4 cursor = ram(DispCursor);
                if (cursor.b & 1) {
                    // over whatever the display shows; image words with a zero top byte are clear.
                    // Under the host's own pointer when it is here (the guest says where the hot
                    // spot is in the image): the guest's position is a few frames behind.
                    int2 corner = int2(asint(cursor.r), asint(cursor.g));
                    if (_HostPointer.z > 0.5) corner = int2(_HostPointer.xy) - int2((cursor.b >> 8) & 31, (cursor.b >> 16) & 31);
                    int2 d = int2(floor(q)) - corner;
                    if (d.x >= 0 && d.y >= 0 && d.x < 32 && d.y < 32) {
                        uint v = ram_word(cursor.a + 4 * (uint)(d.y * 32 + d.x));
                        if ((v >> 24) != 0) {
                            c = rgb(v);
                            done = true;
                        }
                    }
                }
                if (!done && _Raw > 0.5) {
                    c = picture(q, ctrl);
                    done = true;
                }
                if (!done) {
                    // four taps over the screen pixel's footprint: sharp when magnified, and thin
                    // lines survive when the picture is shown smaller than it is
                    float2 foot = spread * fit * size * 0.25;
                    for (uint k = 0; k < 4; k++) {
                        float2 t = clamp(q + (float2(k & 1, k >> 1) * 2.0 - 1.0) * foot, 0, size - 1);
                        c += picture(t, ctrl);
                    }
                    c *= 0.25;
                }
                c *= _Brightness;
#ifndef UNITY_COLORSPACE_GAMMA
                c = GammaToLinearSpace(c);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
