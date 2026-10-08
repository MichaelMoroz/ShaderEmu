Shader "ShaderEmu/Scope"
{
    // The memory screen's other pictures (EmuScope.cs picks one): the CPU's state texels, the
    // ROM, and the sound card's ring of samples as two traces.
    Properties
    {
        _Mode ("1: CPU state, 2: ROM, 3: sound", Int) = 1
        _State ("Machine state texture", 2D) = "black" {}
        _Data_MTD_R ("ROM, first word of each texel", 2D) = "black" {}
        _Mix ("Sound card's mix", 2D) = "black" {}
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
            Texture2D<float4> _Data_MTD_R;
            Texture2D<float4> _Mix;
            uint _Mode;

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

            uint peak(uint v) {
                return max(max(v & 0xff, (v >> 8) & 0xff), max((v >> 16) & 0xff, v >> 24));
            }

            // The CPU's 64 x 64 state texels (registers, CSRs, the UART, the write cache's list,
            // the TLBs), a square each, coloured as the memory view colours RAM.
            float3 cpu_state(float2 p) {
                float2 at = p * 64.0;
                int2 cell = int2(min((int)at.x, 63), min((int)at.y, 63));
                uint4 t = _State.Load(int3(cell, 0));
                float2 f = frac(at);
                float edge = (f.x < 0.08 || f.y < 0.08) ? 0.6 : 1.0;
                if (!any(t != 0)) return float3(0.02, 0.025, 0.04) * edge;
                return (float3(peak(t.r), peak(t.g), peak(t.b | t.a)) / 255.0 * 0.88 + 0.12) * edge;
            }

            // The ROM in three strips, low addresses at the top left: a texel's first three
            // bytes as its colour. Its textures have their first row at the bottom.
            float3 rom(float2 p) {
                uint2 dim;
                _Data_MTD_R.GetDimensions(dim.x, dim.y);
                float sx = p.x * 3.0;
                int strip = min((int)sx, 2);
                int x = min((int)(frac(sx) * (float)dim.x), (int)dim.x - 1);
                int row = (int)(((float)strip + p.y) * (float)dim.y / 3.0);
                if (row >= (int)dim.y) return float3(0.02, 0.025, 0.04);
                float3 c = _Data_MTD_R.Load(int3(x, (int)dim.y - 1 - row, 0)).rgb;
                return c * 0.9 + 0.04;
            }

            // The ring of 16,384 samples as it was last mixed: left above, right below.
            float3 sound(float2 p) {
                float3 c = float3(0.02, 0.025, 0.04);
                bool lower = p.y >= 0.5;
                float y = 1.0 - frac(p.y * 2.0) * 2.0;   // +1 at the top of a trace's half, -1 at its bottom
                if (abs(y) < 0.01) c = float3(0.12, 0.14, 0.18);
                int first = (int)(p.x * 16384.0);
                float lo = 1.0, hi = -1.0;
                for (int k = 0; k < 12; k++) {
                    int n = min(first + k, 16383);
                    float4 s = _Mix.Load(int3(n & 127, n >> 7, 0));
                    float v = clamp((lower ? s.g : s.r) * 4.0, -1.0, 1.0);   // the mix is at the visitor's volume: shown larger
                    lo = min(lo, v);
                    hi = max(hi, v);
                }
                if (y >= lo - 0.02 && y <= hi + 0.02) c = lower ? float3(1.0, 0.6, 0.2) : float3(0.4, 1.0, 0.5);
                return c;
            }

            float4 frag(v2f i) : SV_Target {
                float2 p = float2(i.uv.x, 1.0 - i.uv.y);
                float3 c = _Mode == 1 ? cpu_state(p) : _Mode == 2 ? rom(p) : sound(p);
#ifndef UNITY_COLORSPACE_GAMMA
                c = GammaToLinearSpace(c);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
