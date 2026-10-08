Shader "Nix/rvc"
{
    Properties
    {
        [ToggleUI] _Init ("Init", float) = 0
        [ToggleUI] _InitRaw ("Init directly from _Data_RAM_RAW", float) = 0
        _Ticks ("Ticks per Frame", Int) = 1024
        _TicksDivisor ("Tick Divisor", Int) = 4

        _UdonUARTInChar ("UART input Udon side", Int) = 0
        _UdonUARTInTag ("UART input unique tag", Int) = 0
        _UartBurst ("UART input characters per tag step this shader accepts (host hint)", Int) = 4

        _PlayerID ("Player ID", Int) = -1
        _RTC0 ("RTC0", Int) = 0
        _RTC1 ("RTC1", Int) = 0

        _Data_RAM_R ("RAM Texture R", 2D) = "black" {}
        _Data_RAM_G ("RAM Texture G", 2D) = "black" {}
        _Data_RAM_B ("RAM Texture B", 2D) = "black" {}
        _Data_RAM_A ("RAM Texture A", 2D) = "black" {}

        _Data_RAM_RAW ("RAM Texture RAW", 2D) = "black" {}

        _Data_DTB_R ("Device Tree Binary Texture R", 2D) = "black" {}
        _Data_DTB_G ("Device Tree Binary Texture G", 2D) = "black" {}
        _Data_DTB_B ("Device Tree Binary Texture B", 2D) = "black" {}
        _Data_DTB_A ("Device Tree Binary Texture A", 2D) = "black" {}

        _Data_MTD_R ("MTD ROM Texture R", 2D) = "black" {}
        _Data_MTD_G ("MTD ROM Texture G", 2D) = "black" {}
        _Data_MTD_B ("MTD ROM Texture B", 2D) = "black" {}
        _Data_MTD_A ("MTD ROM Texture A", 2D) = "black" {}

        _DoTick ("DEBUG: Perform single step", Int) = 0
        /* _BreakpointLow ("DEBUG: Breakpoint: Enter single step mode at this PC address (low 16)", Int) = 0 */
        /* _BreakpointHigh ("DEBUG: Breakpoint: Enter single step mode at this PC address (high 16)", Int) = 0 */
        /* _BreakpointLowClock ("DEBUG: Breakpoint: Enter single step mode at this clock (low 16)", Int) = 0 */
        /* _BreakpointHighClock ("DEBUG: Breakpoint: Enter single step mode at this clock (high 16)", Int) = 0 */
        /* _CheckCSR ("DEBUG: Display CSR", Int) = 0 */
        /* _CheckMEM ("DEBUG: Display RAM", Int) = 0 */
        /* [ToggleUI] _CheckMEMraw ("DEBUG: Display RAM raw (not | 0x80000000)", Int) = 0 */
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "CPUTick"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex CustomRenderTextureVertexShader
            #pragma fragment frag

            #define PASS_TICK

            // Cache buster: 31

            // custom crt include w/ Texture2D<uint4> self-reference
            #include "crt.cginc"
            #include "UnityCG.cginc"

            uniform uint _Init, _InitRaw;
            uniform uint _Ticks, _TicksDivisor;
            uniform uint _UdonUARTInChar, _UdonUARTInTag;
            uniform uint _PlayerID;
            uniform uint _RTC0, _RTC1;
            uniform uint _DoTick;
            /* uniform uint _BreakpointLow, _BreakpointHigh; */
            /* uniform uint _BreakpointLowClock, _BreakpointHighClock; */
            /* uniform uint _CheckCSR; //, _CheckMEM, _CheckMEMraw; */
            /* static uint _Breakpoint, _BreakpointClock; */

            Texture2D<float4> _Data_DTB_R;
            Texture2D<float4> _Data_DTB_G;
            Texture2D<float4> _Data_DTB_B;
            Texture2D<float4> _Data_DTB_A;

            Texture2D<float4> _Data_MTD_R;
            Texture2D<float4> _Data_MTD_G;
            Texture2D<float4> _Data_MTD_B;
            Texture2D<float4> _Data_MTD_A;

            static uint hart = 0;
            static uint2 hart_offset = uint2(0, 0);

#if defined(SHADER_STAGE_COMPUTE) && defined(RAM_BUFFER)
            // RAM_BUFFER: RAM is a byte address buffer of the compute tick's (imported from the
            // state texture once, by ram_import_cs): a load is one word or four, a store one
            // word, and there is no write cache. Nothing but the tick sees this RAM, so only
            // programs that need no device but the console run.
            #define RAM_BUFFER_ON
            RWByteAddressBuffer _RamB : register(u1);
            #define RAM_TEXEL(t) (_RamB.Load4((t) << 4))
#endif
#ifndef RAM_BUFFER_ON
            #define RAM_TEXEL(t) STATE_TEX(RAM_ADDR(t))
#endif
#if defined(SHADER_STAGE_COMPUTE) && defined(RAM_DIRECT)
            // RAM_DIRECT: the compute tick has the whole state texture as a UAV. It reads it and
            // writes it: a store goes to RAM there and then, and there is no write cache.
            #define RAM_DIRECT_ON
            RWTexture2D<uint4> _Ram;
            #define STATE_TEX_HART(pos, hartidx) (_Ram[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (_Ram[pos])
#else
            #define STATE_TEX_HART(pos, hartidx) (_SelfTexture2D[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (_SelfTexture2D[pos])
#endif

            static uint2 s_dim;
            static uint2 m_dim;

            #include "helpers.cginc"
            #include "src/prof.h"

            #include "src/types.h"
            #include "src/ins.h"
            #include "src/uart.h"
            #include "src/emu.h" // includes mmu.h, csr.h, mem.h, trap.h
            #include "src/cpu.h"

            uint4 frag(v2f_customrendertexture i) : SV_Target {
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);

                uint2 pos = i.globalTexcoord.xy * s_dim;
#ifdef L1_LOCAL
                uint4 l1_cache[L1_DATA_N];
#if L1_WAYS == 4
                uint4 l1_tag[L1_BUCKETS];
#endif
#ifndef NO_PAGING
                uint tlb2_tag[3 * TLB2_N];
                uint tlb2_pg[3 * TLB2_N];
#endif
#endif

                _Ticks /= max(_TicksDivisor, 1);
                _Ticks = max(_Ticks, 2);

                // calculate active hart in 2x2 (mip-map) grid
                /* hart = pos.y % 2 == 0 ? */
                /*     (pos.x % 2 == 0 ? 0 : 1) : */
                /*     (pos.x % 2 == 0 ? 2 : 3); */
                /* hart_offset = uint2(hart % 2, hart / 2); */
                /* pos /= 2; */
                /* if (hart > 0) { */
                /*     // FIXME */
                /*     return uint4(~0, ~0, ~0, ~0); */
                /* } */

                /* _Breakpoint = _BreakpointLow | (_BreakpointHigh << 16); */
                /* _BreakpointClock = _BreakpointLowClock | (_BreakpointHighClock << 16); */

                if (_Init) {
                    if (_InitRaw) {
                        return _SelfTexture2D[pos];
                    } else {
                        cpu = cpu_init();
                    }
                } else {
                    if (!pixel_has_state(pos)) {
                        return STATE_TEX(pos);
                    }

                    decode();
                    time_prepare();
                    xreg_load();
#ifdef FPU
                    fp_state_load();
#endif
#ifndef NO_PAGING
                    tlb_state_load();
#endif

                    uint i = 0;
                    [loop]
                    while (i < _Ticks && !cpu.stall) {
                        // as many fast ticks in a row as possible, then one general tick
                        i += fast_run(_Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            i++;
                        }
                    }
                    xreg_store();
                    #ifdef PROFILE
                    prof_flush(pos);
                    #endif
                }

                /* cpu.debug_csr_val = read_csr_raw(_CheckCSR); */
                /* cpu.debug_mem_val = mem_get_word(_CheckMEM | (_CheckMEMraw ? 0 : 0x80000000)); */

#ifndef NO_PAGING
                uint4 tlb_texel;
                if (!_Init && tlb_state_texel(pos, tlb_texel)) return tlb_texel;
#endif
#ifdef FPU
                uint4 fp_texel;
                if (!_Init && fp_state_texel(pos, fp_texel)) return fp_texel;
#endif
                return encode(pos);
            }
#ifdef SHADER_STAGE_COMPUTE
            // The experiment: the same tick as one thread of a compute shader. It writes every
            // texel the pass owns into _TickOut (the 64 x 64 CPU zone, which the host fills with
            // the state's before the dispatch and copies back after it).
#ifdef RAM_DIRECT_ON
            #define _TickOut _Ram
#else
            RWTexture2D<uint4> _TickOut : register(u0);
#endif

#ifdef RAM_BUFFER_ON
            // 64 threads a group, 65,536 texels a row of groups: every RAM texel into the buffer
            [numthreads(64, 1, 1)]
            void ram_import_cs(uint3 thread : SV_DispatchThreadID) {
                uint t = thread.x + thread.y * 65536;
                if (t < RAM_MAX / 16) {
                    _RamB.Store4(t << 4, _SelfTexture2D[RAM_ADDR(t)]);
                }
            }
#endif

            [numthreads(1, 1, 1)]
            void tick_cs(uint3 thread : SV_DispatchThreadID) {
#ifdef RAM_DIRECT_ON
                _Ram.GetDimensions(s_dim.x, s_dim.y);
#else
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
#endif
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);
#ifdef L1_LOCAL
                uint4 l1_cache[L1_DATA_N];
#if L1_WAYS == 4
                uint4 l1_tag[L1_BUCKETS];
#endif
#ifndef NO_PAGING
                uint tlb2_tag[3 * TLB2_N];
                uint tlb2_pg[3 * TLB2_N];
#endif
#endif


                _Ticks /= max(_TicksDivisor, 1);
                _Ticks = max(_Ticks, 2);

                decode();
                time_prepare();
                xreg_load();
#ifndef NO_PAGING
                tlb_state_load();
#endif

                uint i = 0;
                [loop]
                while (i < _Ticks && !cpu.stall) {
                    i += fast_run(_Ticks - i);
                    if (i < _Ticks && !cpu.stall) {
                        cpu_tick_l1(L1A0);
                        i++;
                    }
                }
                xreg_store();

#ifdef RAM_BUFFER_ON
                // The machine's copy and fill (the Commit pass does them on the texture's RAM):
                // here on the buffer, the instruction having ended the pass. A copy within RAM
                // to a higher address goes backwards, as one from the texture's old contents would.
                if (cpu.stall == STALL_MEMOP_COPY || cpu.stall == STALL_MEMOP_FILL) {
                    bool fill = cpu.stall == STALL_MEMOP_FILL;
                    uint first = (cpu.memop_dst_p + 3) & ~3u, end = cpu.memop_dst_p + cpu.memop_n;
                    uint words = end > first ? (end - first + 3) >> 2 : 0;
                    bool back = !fill && (cpu.memop_src_p & 0x80000000) != 0 && cpu.memop_dst_p > (cpu.memop_src_p & 0x7fffffff);
                    [loop]
                    for (uint k = 0; k < words; k++) {
                        uint at = first + (back ? words - 1 - k : k) * 4;
                        uint v = cpu.memop_src_v;
                        [branch]
                        if (!fill) {
                            v = mem_get_word((cpu.memop_src_p + (at - cpu.memop_dst_p)) & ~3u);
                        }
                        [branch]
                        if (at < RAM_MAX) {
                            _RamB.Store(at, v);
                        }
                    }
                }
#endif

                uint lin;
                [loop]
                for (lin = 0; lin < 44; lin++) {
                    uint2 p = uint2(lin % 64, lin / 64);
                    _TickOut[p] = encode(p);
                }
                [loop]
                for (lin = 1068; lin < 1068 + L1_ENTRIES; lin++) {
                    uint2 p = uint2(lin % 64, lin / 64);
                    _TickOut[p] = encode(p);
                }
#ifndef NO_PAGING
                [loop]
                for (lin = TLB_STATE_AT; lin < TLB_STATE_AT + TLB_STATE_TEXELS; lin++) {
                    uint2 p = uint2(lin % 64, lin / 64);
                    uint4 tlb_texel;
                    tlb_state_texel(p, tlb_texel);
                    _TickOut[p] = tlb_texel;
                }
#endif
            }
#endif
            ENDCG
        }

        Pass
        {
            Name "Commit"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex commit_vert
            #pragma fragment frag

            #define PASS_COMMIT

            // Cache buster: 31

            // custom crt include
            #include "crt.cginc"
            #include "UnityCG.cginc"

            float _Init, _InitRaw;

            Texture2D<float4> _Data_RAM_R;
            Texture2D<float4> _Data_RAM_G;
            Texture2D<float4> _Data_RAM_B;
            Texture2D<float4> _Data_RAM_A;
            Texture2D<float4> _Data_RAM_RAW;

            Texture2D<float4> _Data_MTD_R;
            Texture2D<float4> _Data_MTD_G;
            Texture2D<float4> _Data_MTD_B;
            Texture2D<float4> _Data_MTD_A;

            #define STATE_TEX_HART(pos, hartidx) (_SelfTexture2D[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (_SelfTexture2D[pos])

            static uint2 s_dim;
            static uint2 m_dim;

            #include "helpers.cginc"
            #include "src/types.h" // includes fb.h

#ifdef GPU_DEVICE
            // The GPU device's picture, for copying into the RAM framebuffer (docs/gpu.md).
            Texture2D<float4> _GpuTarget;
            #define GPU_STATE _SelfTexture2D
            #define GPU_WRITEBACK
            #include "src/gpu.h"
#endif

#ifdef COMMIT_BANDS
#if RAM_TILE_BITS != 0
#error COMMIT_BANDS needs RAM in row order
#endif
            // RAM in 4 MB bands (128 rows), one bit each: those this commit changes. The other
            // buffer holds the state of two commits ago, so a band neither this commit nor the
            // last one changes is already right there and is not drawn.
            uint commit_bands_changed() {
                uint stalled = STATE_TEX_HART(uint2(28, 0), 0).r;
                if (_Init) return 0xffffffff;
                uint changed = STATE_TEX_HART(uint2(41, 0), 0).g | (1u << 28);   // the tick's writes; GPU control words
                if (stalled == STALL_MEMOP_COPY || stalled == STALL_MEMOP_FILL) {
                    // a parallel copy or fill: the bands from its destination to its end
                    uint4 op = STATE_TEX_HART(uint2(38, 0), 0);
                    uint lo = (op.g >> 22) & 31, hi = min((op.g + op.b - 1) >> 22, 31u);
                    if (op.b != 0 && hi >= lo) changed |= ((2u << hi) - 1) & ~((1u << lo) - 1);
                }
#ifdef GPU_DEVICE
                changed |= gpu_copy_bands();
#endif
                return changed;
            }
#endif

            // One quad for the whole texture, or with COMMIT_BANDS (33 quads) the state rows and
            // each band of RAM that has to be drawn.
            v2f_customrendertexture commit_vert(appdata_customrendertexture IN) {
#ifdef COMMIT_BANDS
                static const float2 corners[6] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}};
                uint quad = IN.vertexID / 6;
                float2 corner = corners[IN.vertexID % 6];
                float top = quad == 0 ? 0.0 : 64.0 + (quad - 1) * 128.0, rows = quad == 0 ? 64.0 : 128.0;
                uint drawn = commit_bands_changed() | STATE_TEX_HART(uint2(41, 0), 0).b;
                bool on = quad == 0 || ((drawn >> ((quad - 1) & 31)) & 1) != 0;
                float2 uv = float2(corner.x, (top + corner.y * rows) / _CustomRenderTextureInfo.y);
                v2f_customrendertexture OUT;
                OUT.vertex = on ? float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0) : float4(2.0, 2.0, 0.0, 1.0);
                OUT.localTexcoord = float3(uv, 0);
                OUT.globalTexcoord = float3(uv, 0);
                OUT.primitiveID = 0;
                OUT.direction = 0;
                return OUT;
#else
                return CustomRenderTextureVertexShader(IN);
#endif
            }

            uint4 frag(v2f_customrendertexture i) : SV_Target {
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);
                uint2 pos = i.globalTexcoord.xy * s_dim;

                if (_Init && _InitRaw) {
                    float3 raw[6];
                    for (uint off = 0; off < 6; off++) {
                        raw[off] = _Data_RAM_RAW[pos + uint2((off * s_dim.x) % (s_dim.x*3), (off / 3) * s_dim.y)].rgb;
                    }
                    uint4 data = unpack_uint4(raw);
                    return data;
                }

#ifdef GPU_DEVICE
                uint4 picture;
                if (gpu_writeback(pos, picture)) return picture;
#endif
                decode_for_commit();
                uint4 result = commit(pos);
#ifdef COMMIT_BANDS
                if (pos.x == 41 && pos.y == 0) result.b = commit_bands_changed();   // for the next commit
#endif
                return result;
            }
            ENDCG
        }
    }
}
