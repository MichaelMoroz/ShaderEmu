Shader "ShaderEmu/Scope"
{
    // The memory screen's other pictures (EmuScope.cs picks one): the CPU's state texels, the
    // ROM, the sound card's ring of samples as two traces, and the GPU's memory (docs/machine/gpu.md):
    // the VRAM (its slots and the rows gles.c streams into), the frame buffer and the vertices' draw IDs.
    Properties
    {
        _Mode ("1: CPU state, 2: ROM, 3: sound, 4: the GPU's memory", Int) = 1
        _State ("Machine state texture", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _Mix ("Sound card's mix", 2D) = "black" {}
        _GpuVram ("The GPU's VRAM (4096 x 4096 RGBA8)", 2D) = "black" {}
        _GpuTarget ("The GPU's colour target, its frame buffer (1280 x 720)", 2D) = "black" {}
        _GpuDrawIds ("The draw IDs: a texel a mesh vertex, its command's texel and its passes (512 x 384 RGInt)", 2D) = "black" {}
        _Aspect ("The screen's width over its height (the VRAM is drawn square at its left)", Float) = 2
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
            Texture2D<float4> _GpuVram;
            SamplerState sampler_GpuVram_linear_clamp;
            Texture2D<float4> _GpuTarget;
            Texture2D<int2> _GpuDrawIds;
            float _Aspect;
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
                uint4 geo = _State.Load(int3(0x3d, 64 + 0x100000 / 2048, 0));
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
                uint w = asuint(_Data_ROM.Load(int3(x, row, 0)).x);
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

            // The VRAM square at the screen's left, its row 0 at the top as the GPU addresses it (gpu.h's
            // VRAM_X and VRAM_Y). Level 0 in four bilinear taps a pixel: the levels are made only when a list
            // asks for them. A texel's alpha is shown over a dim checker of 64 texels, a 512-texel grid over all.
            float3 vram(float2 t, float2 d) {
                float4 c = 0;
                c += _GpuVram.SampleLevel(sampler_GpuVram_linear_clamp, t + float2(-d.x, -d.y), 0);
                c += _GpuVram.SampleLevel(sampler_GpuVram_linear_clamp, t + float2( d.x, -d.y), 0);
                c += _GpuVram.SampleLevel(sampler_GpuVram_linear_clamp, t + float2(-d.x,  d.y), 0);
                c += _GpuVram.SampleLevel(sampler_GpuVram_linear_clamp, t + float2( d.x,  d.y), 0);
                c *= 0.25;
                float2 texel = t * 4096;
                float check = ((uint(texel.x / 64) + uint(texel.y / 64)) & 1) ? 0.07 : 0.04;
                float3 rgb = lerp(check.xxx, c.rgb, c.a);
                float2 grid = abs(frac(texel / 512 + 0.5) - 0.5) * 512;
                return min(grid.x, grid.y) < abs(d.x) * 4096 * 2 ? lerp(rgb, float3(0.25, 0.35, 0.5), 0.6) : rgb;
            }

            // The frame buffer as the display reads it (Display.shader's mode 3: its pixel (0, 0) at the top left),
            // four pixels averaged.
            float3 frame_buffer(float2 t, float2 d) {
                float2 size = float2(1280, 720), at = t * size, e = max(d * size, 0.5);
                float3 c = _GpuTarget.Load(int3(clamp(at + float2(-e.x, -e.y), 0, size - 1), 0)).rgb
                         + _GpuTarget.Load(int3(clamp(at + float2( e.x, -e.y), 0, size - 1), 0)).rgb
                         + _GpuTarget.Load(int3(clamp(at + float2(-e.x,  e.y), 0, size - 1), 0)).rgb
                         + _GpuTarget.Load(int3(clamp(at + float2( e.x,  e.y), 0, size - 1), 0)).rgb;
                return c * 0.25;
            }

            // A vertex's draw ID: a colour of its command's texel, bright where it is drawn in a pass, dim where in none.
            float3 draw_ids(float2 t) {
                int2 v = _GpuDrawIds.Load(int3(clamp(int2(t * float2(512, 384)), int2(0, 0), int2(511, 383)), 0));
                if (v.x == 0 && v.y == 0) return float3(0.02, 0.025, 0.04);
                uint h = (uint)v.x * 2654435761u;
                float3 c = float3((h >> 8) & 255, (h >> 16) & 255, h >> 24) / 255.0 * 0.7 + 0.3;
                return v.y != 0 ? c : c * 0.3;
            }

            // The GPU's memory: the VRAM a square at the left, the frame buffer at the top of what is left, the draw IDs
            // under it, each at its own aspect, apart by a dark gap.
            float3 gpu_memory(float2 p) {
                const float3 back = float3(0.02, 0.025, 0.04);
                float2 q = float2(p.x * _Aspect, p.y);   // in the screen's heights
                float2 d = float2(ddx(q.x), ddy(q.y)) * 0.25;   // (before any branch: no derivatives inside one)
                if (q.x < 1.0) return vram(q, d);
                float left = 1.0 + 0.02, wide = _Aspect - left;
                float2 u = float2(q.x - left, q.y);
                if (u.x < 0) return back;
                float fbHigh = wide * 720.0 / 1280.0;
                if (u.y < fbHigh) return frame_buffer(float2(u.x / wide, u.y / fbHigh), d / float2(wide, fbHigh));
                float top = fbHigh + 0.02, idHigh = 1.0 - top, idWide = idHigh * 512.0 / 384.0;
                if (u.y < top || u.x > idWide) return back;
                return draw_ids(float2(u.x / idWide, (u.y - top) / idHigh));
            }

            float4 frag(v2f i) : SV_Target {
                float2 p = float2(i.uv.x, 1.0 - i.uv.y);
                float3 c = _Mode == 1 ? cpu_state(p) : _Mode == 2 ? rom(p) : _Mode == 4 ? gpu_memory(p) : sound(p);
#ifndef UNITY_COLORSPACE_GAMMA
                c = GammaToLinearSpace(c);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
