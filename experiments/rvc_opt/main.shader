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

            // CORES > 1 (docs/multicore.md): core k's state is the block at x = CORE_PITCH * k of the
            // state rows, and state_off is where the block of this pixel's core starts (mc_core is
            // the core). RAM is read with RAM_TEX, which no core's block moves.
            #ifndef CORES
            #define CORES 1
            #endif
            #ifndef CORE_PITCH
            #define CORE_PITCH 256
            #endif
            #define MC_BLOCK(k) uint2((k) * CORE_PITCH, 0)
            #if CORES > 1
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            #define STATE_TEX_HART(pos, hartidx) (_SelfTexture2D[uint2(pos) + state_off])
            #define STATE_TEX(pos) (_SelfTexture2D[uint2(pos) + state_off])
            #else
            #define STATE_TEX_HART(pos, hartidx) (_SelfTexture2D[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (_SelfTexture2D[pos])
            #endif
            #define RAM_TEX(pos) (_SelfTexture2D[pos])

            static uint2 s_dim;
            static uint2 m_dim;

            #include "helpers.cginc"
            #include "src/prof.h"

            #include "src/types.h"
            #include "src/mc.h"
            #include "src/ins.h"
            #include "src/uart.h"
            #include "src/emu.h" // includes mmu.h, csr.h, mem.h, trap.h
            #include "src/cpu.h"

            uint4 frag(v2f_customrendertexture i) : SV_Target {
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);

                uint2 pos = i.globalTexcoord.xy * s_dim;
#if CORES > 1
                // a zone or two a core: this pixel's core, and which texel of its block it keeps
                uint2 own = pos;
                hart = min(pos.x / CORE_PITCH, CORES - 1);
                mc_core = hart;
                state_off = MC_BLOCK(hart);
                pos -= state_off;
                if (pos.x >= 64 || pos.y >= 64) return (uint4)0;   // between two blocks
                pos = mc_texel_of(pos);
#endif
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
                        return STATE_TEX(pos);
                    } else {
                        cpu = cpu_init();
                    }
                } else {
                    if (!pixel_has_state(pos)) {
#if CORES > 1
                        return RAM_TEX(own);
#else
                        return STATE_TEX(pos);
#endif
                    }

                    decode();
                    time_prepare();
#if CORES > 1
                    mc_enter();
#endif
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

            // CORES > 1 (docs/multicore.md): core k's state is the block at x = CORE_PITCH * k of the
            // state rows, and state_off is where the block of this pixel's core starts (mc_core is
            // the core). RAM is read with RAM_TEX, which no core's block moves.
            #ifndef CORES
            #define CORES 1
            #endif
            #ifndef CORE_PITCH
            #define CORE_PITCH 256
            #endif
            #define MC_BLOCK(k) uint2((k) * CORE_PITCH, 0)
            #if CORES > 1
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            #define STATE_TEX_HART(pos, hartidx) (_SelfTexture2D[uint2(pos) + state_off])
            #define STATE_TEX(pos) (_SelfTexture2D[uint2(pos) + state_off])
            #else
            #define STATE_TEX_HART(pos, hartidx) (_SelfTexture2D[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (_SelfTexture2D[pos])
            #endif
            #define RAM_TEX(pos) (_SelfTexture2D[pos])

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
#if CORES > 1
            uint commit_bands_changed_core();
            // every core's writes
            uint commit_bands_changed() {
                uint changed = 0;
                for (uint h = 0; h < CORES; h++) {
                    state_off = MC_BLOCK(h);
                    changed |= commit_bands_changed_core();
                }
                state_off = uint2(0, 0);
                return changed;
            }
            uint commit_bands_changed_core() {
#else
            uint commit_bands_changed() {
#endif
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
#if CORES > 1
                uint4 result;
                if (pos.y < 64) {
                    // a core's own block: its state, as with one core
                    uint hb = pos.x / CORE_PITCH;
                    if (hb >= CORES || pos.x % CORE_PITCH >= 64) return (uint4)0;
                    uint4 own = RAM_TEX(pos);
                    mc_core = hb;
                    state_off = MC_BLOCK(hb);
                    pos = mc_texel_of(pos - state_off);
                    decode_for_commit();
                    result = commit(pos, own);
                } else {
                    // RAM: every core's writes, the highest core's last
                    result = RAM_TEX(pos);
                    for (uint h = 0; h < CORES; h++) {
                        mc_core = h;
                        state_off = MC_BLOCK(h);
                        decode_for_commit();
                        result = commit(pos, result);
                    }
                    mc_core = 0;
                    state_off = uint2(0, 0);
                }
#else
                decode_for_commit();
                uint4 result = commit(pos, STATE_TEX_HART(pos, 0));
#endif
#ifdef COMMIT_BANDS
                if (pos.x == 41 && pos.y == 0) result.b = commit_bands_changed();   // for the next commit
#endif
                return result;
            }
            ENDCG
        }
    }
}
