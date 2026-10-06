Shader "ShaderEmu/Terminal"
{
    // A character terminal. _Grid has one texel per cell: character, foreground and background
    // colour numbers (0-15) in r, g, b. EmuTerminal.cs fills it; rows are a ring starting at
    // _RowOffset.
    Properties
    {
        _Grid ("Character grid", 2D) = "black" {}
        _Font ("Glyph atlas: ASCII 32-127, 16 columns", 2D) = "black" {}
        _Cols ("Columns", Int) = 80
        _Rows ("Rows", Int) = 30
        _RowOffset ("Grid row shown at the top", Int) = 0
        _Cursor ("Cursor column, row, visible", Vector) = (0, 0, 1, 0)
        _Margin ("Margin, as a part of the quad", Float) = 0.015
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

            Texture2D<float4> _Grid;
            sampler2D _Font;
            uint _Cols, _Rows, _RowOffset;
            float4 _Cursor;
            float _Margin;

            static const float3 Palette[16] = {
                float3(0.035, 0.04, 0.05), float3(0.80, 0.25, 0.25), float3(0.45, 0.78, 0.35), float3(0.85, 0.68, 0.30),
                float3(0.35, 0.55, 0.90), float3(0.70, 0.45, 0.85), float3(0.35, 0.75, 0.80), float3(0.82, 0.84, 0.86),
                float3(0.35, 0.37, 0.42), float3(1.00, 0.42, 0.42), float3(0.60, 0.95, 0.50), float3(1.00, 0.85, 0.45),
                float3(0.50, 0.70, 1.00), float3(0.85, 0.60, 1.00), float3(0.50, 0.92, 0.95), float3(1.00, 1.00, 1.00)
            };

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

            float4 frag(v2f i) : SV_Target {
                float3 c = Palette[0];
                float2 p = (float2(i.uv.x, 1.0 - i.uv.y) - _Margin) / (1.0 - 2.0 * _Margin);
                float2 at = p * float2(_Cols, _Rows);
                // gradients from the unbroken coordinate, so cell edges do not pick a small mip
                float2 gx = ddx(at) / float2(16, 6), gy = ddy(at) / float2(16, 6);
                if (p.x >= 0 && p.y >= 0 && p.x < 1 && p.y < 1) {
                    uint2 cell = min((uint2)at, uint2(_Cols, _Rows) - 1);
                    float2 f = at - cell;
                    uint3 t = (uint3)(_Grid.Load(int3(cell.x, (cell.y + _RowOffset) % _Rows, 0)).rgb * 255.0 + 0.5);
                    uint ch = t.r < 32 || t.r > 126 ? 0 : t.r - 32;
                    float3 fg = Palette[t.g & 15], bg = Palette[t.b & 15];
                    if (_Cursor.z > 0.5 && cell.x == (uint)_Cursor.x && cell.y == (uint)_Cursor.y && frac(_Time.y * 1.5) < 0.5) {
                        float3 swap = fg;
                        fg = bg;
                        bg = swap;
                    }
                    float2 glyph = (float2(ch % 16, ch / 16) + f) / float2(16, 6);
                    float ink = tex2Dgrad(_Font, float2(glyph.x, 1.0 - glyph.y), gx, float2(gy.x, -gy.y)).r;
                    c = lerp(bg, fg, ink);
                }
#ifndef UNITY_COLORSPACE_GAMMA
                c = GammaToLinearSpace(c);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
