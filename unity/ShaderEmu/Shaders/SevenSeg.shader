Shader "ShaderEmu/SevenSeg"
{
    // The speed's window on a computer's tower (docs/stations.md): two rows of three digits of
    // seven bars each, with a point before the last. The upper row is every core's millions of
    // instructions a second, the lower core 0's. A row's number is in tenths; below zero it is dark.
    Properties
    {
        _All ("Upper row, in tenths (below 0: dark)", Float) = -1
        _Core ("Lower row, in tenths (below 0: dark)", Float) = -1
        _Lit ("A lit bar", Color) = (1, 0.5, 0.08, 1)
        _Dark ("An unlit bar", Color) = (0.06, 0.035, 0.02, 1)
        _Back ("The window", Color) = (0.012, 0.012, 0.014, 1)
        _Glow ("Brightness of a lit bar (above 1 it blooms)", Float) = 4
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            float _All, _Core, _Glow;
            float4 _Lit, _Dark, _Back;

            struct v2f {
                float4 pos : SV_Position;
                float2 uv : TEXCOORD0;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata_base v) {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.texcoord.xy;
                return o;
            }

            // How much of a bar from a to b, `thick` across, covers p (1 inside, soft at its edge).
            float bar(float2 p, float2 a, float2 b, float thick, float soft) {
                float2 along = b - a;
                float t = saturate(dot(p - a, along) / dot(along, along));
                return 1.0 - smoothstep(thick - soft, thick + soft, length(p - a - along * t));
            }

            // Which bars digit d lights: bit 0 the top, then clockwise, bit 6 the middle.
            static const int bars[10] = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };

            float4 frag(v2f i) : SV_Target {
                float soft = max(fwidth(i.uv.x) * 3.0, 0.004);
                bool upper = i.uv.y > 0.5;
                float value = upper ? _All : _Core;
                int tenths = (int)clamp(value + 0.5, 0, 999);
                // a row: three cells across, a margin round them
                float2 p = float2(i.uv.x, frac(i.uv.y * 2.0));
                p = (p - float2(0.06, 0.14)) / float2(0.88, 0.72);
                int cell = (int)floor(p.x * 3.0);
                float2 q = float2(frac(p.x * 3.0), p.y);   // within the cell: x right, y up
                bool there = p.x >= 0.0 && p.x < 1.0 && p.y >= 0.0 && p.y <= 1.0;
                int digit = cell == 0 ? tenths / 100 : cell == 1 ? (tenths / 10) % 10 : tenths % 10;
                int lit = value < 0.0 || (cell == 0 && tenths < 100) ? 0 : bars[digit];
                // the seven bars, in a cell 0.72 wide
                float x0 = 0.16, x1 = 0.66, y0 = 0.04, y1 = 0.5, y2 = 0.96, w = 0.055;
                float2 ends[14] = { float2(x0, y2), float2(x1, y2), float2(x1, y2), float2(x1, y1), float2(x1, y1), float2(x1, y0), float2(x1, y0), float2(x0, y0),
                                    float2(x0, y0), float2(x0, y1), float2(x0, y1), float2(x0, y2), float2(x0, y1), float2(x1, y1) };
                float on = 0.0, off = 0.0;
                for (int k = 0; k < 7; k++) {
                    // (a bar stops short of its ends, so two do not run together)
                    float2 a = lerp(ends[2 * k], ends[2 * k + 1], 0.12), b = lerp(ends[2 * k], ends[2 * k + 1], 0.88);
                    float c = bar(q * float2(1.0, 1.6), a * float2(1.0, 1.6), b * float2(1.0, 1.6), w, soft * 3.0);
                    if ((lit >> k) & 1) on = max(on, c);
                    else off = max(off, c);
                }
                // the point, before the last digit
                float dot_ = 1.0 - smoothstep(0.05, 0.05 + soft * 3.0, length((q - float2(0.9, 0.05)) * float2(1.0, 1.6)));
                if (cell == 1) { if (value >= 0.0) on = max(on, dot_); else off = max(off, dot_); }
                float3 c3 = _Back.rgb;
                if (there) c3 = lerp(lerp(c3, _Dark.rgb, off), _Lit.rgb * _Glow, on);
                return float4(c3, 1.0);
            }
            ENDCG
        }
    }
}
