Shader "ShaderEmu/Scope"
{
    // The memory screen's other pictures (EmuScope.cs picks one): the CPU's state texels, the
    // ROM, and the sound card's ring of samples as two traces.
    Properties
    {
        _Mode ("1: CPU state, 2: ROM, 3: sound", Int) = 1
        _State ("Machine state texture", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
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
            Texture2D<float4> _Data_ROM;   // the ROM: a 16-byte texel a pixel, read by its bits (asuint)
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

            // A state texel as a square, coloured as the memory view colours RAM.
            float3 state_cell(uint4 t, float2 f) {
                float edge = (f.x < 0.1 || f.y < 0.1) ? 0.6 : 1.0;
                if (!any(t != 0)) return float3(0.02, 0.025, 0.04) * edge;
                return (float3(peak(t.r), peak(t.g), peak(t.b | t.a)) / 255.0 * 0.88 + 0.12) * edge;
            }

            // Every core's state, as the tick's eight targets leave it in the state texture. Above:
            // core 0's 64 x 32 texels (registers, the UART, the write cache, the TLBs, then its
            // CSRs). Below: the workers, a block each, as many as the machine's geometry has
            // (src/types.cginc: how many, and their size, in the control texel 0x3d), a worker's
            // texels (its registers, its write cache, its float registers) twenty to a row.
            float3 cpu_state(float2 p) {
                const float3 back = float3(0.02, 0.025, 0.04);
                const float top = 0.34;
                if (p.y < top) {
                    float2 at = float2(p.x * 64.0, p.y / top * 32.0);
                    return state_cell(_State.Load(int3(min((int)at.x, 63), min((int)at.y, 31), 0)), frac(at));
                }
                uint4 geo = _State.Load(int3(0x3d, 64 + 0x700000 / 2048, 0));
                uint rows = geo.b >= 3 && geo.b <= 6 ? (52 + ((7u << geo.b) >> 2) * 4 + 63) / 64 : 0;
                uint count = (geo.r & 1) != 0 ? 0 : min(geo.g, 255u);
                if (rows == 0 || count == 0) return back * 0.6;
                uint side = 1;
                for (int n = 0; n < 16; n++) if (side * side < count) side++;
                float2 g = float2(p.x, (p.y - top) / (1.0 - top)) * (float)side;
                uint j = min((uint)g.y, side - 1) * side + min((uint)g.x, side - 1);
                float2 within = (frac(g) - 0.06) / 0.88;
                if (j >= count || any(within < 0.0) || any(within >= 1.0)) return back * 0.6;
                uint texels = rows * 64, down = (texels + 19) / 20;
                float2 at = within * float2(20.0, (float)down);
                uint w = (uint)at.y * 20 + (uint)at.x;
                if (w >= texels) return back * 0.6;
                // the worker's tile and the texel in it: 49 workers of five tiles to a band of 248
                uint per = 248 / rows, tile = (j / per) * 248 + (j % per) * rows + (w >> 6), i6 = w & 63;
                return state_cell(_State.Load(int3(64 + (tile % 248) * 8 + (i6 & 7), (tile / 248) * 8 + (i6 >> 3), 0)), frac(at));
            }

            // The ROM in three strips, low addresses at the top left: a texel's first three
            // bytes as its colour. Its textures have their first row at the bottom.
            float3 rom(float2 p) {
                uint2 dim;
                _Data_ROM.GetDimensions(dim.x, dim.y);
                float sx = p.x * 3.0;
                int strip = min((int)sx, 2);
                int x = min((int)(frac(sx) * (float)dim.x), (int)dim.x - 1);
                int row = (int)(((float)strip + p.y) * (float)dim.y / 3.0);
                if (row >= (int)dim.y) return float3(0.02, 0.025, 0.04);
                uint w = asuint(_Data_ROM.Load(int3(x, (int)dim.y - 1 - row, 0)).x);
                float3 c = float3(w & 0xff, (w >> 8) & 0xff, (w >> 16) & 0xff) / 255.0;
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
