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
            #pragma geometry tick_geom
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

            // CORES > 1 (docs/multicore.md): core 0's state is the block at the left of the state
            // rows and the workers' is the strip beside it, each worker some rows of it, as the
            // geometry says (types.h); state_off is where the state of the core looked at begins
            // (mc_core is the core). RAM is read with RAM_TEX, which no core's block moves.
            #ifndef CORES
            #define CORES 1
            #endif
            #if CORES > 1
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            static uint mc_bits_now = 6;   // the write cache's size of the core looked at (types.h)
            static uint mc_size_now = 4096; // and how many texels of state it has
            static uint mc_tile0 = 0;      // and, a worker's, the first of its tiles
            static uint4 mc_geo;           // the geometry's texel, read once a pixel
            // A worker's state is a strip of whole 8 x 8 tiles in the band beside core 0's block,
            // one worker's after another's (types.h): texel w of it is in its tile w / 64. The
            // tick draws a strip as a quad of its own. What lies past a worker's last texel is
            // another worker's, and reads as the zeros it used to be (a control register a
            // worker never wrote).
            uint4 mc_state_read(uint2 pos) {
                if (mc_core == 0) return _SelfTexture2D[pos + state_off];
                uint w = pos.y * 64 + pos.x;
                if (w >= mc_size_now) return (uint4)0;
                uint t = mc_tile0 + (w >> 6), i = w & 63;
                return _SelfTexture2D[uint2(64 + (t << 3) + (i & 7), i >> 3)];
            }
            #define STATE_TEX_HART(pos, hartidx) mc_state_read(uint2(pos))
            #define STATE_TEX(pos) mc_state_read(uint2(pos))
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

            // What the tick draws: a quad for core 0's rectangle, and one for each worker that
            // has something to run, where the machine's geometry puts its strip. They are
            // rectangles of their own, so no two cores' pixels are rasterised together, and a
            // worker that is parked or asleep costs no pixel at all. (The pass is handed one
            // quad, two triangles: the first becomes these, the second nothing.)
            // TICK_MRT: a pixel of the tick is eight texels of state, one in each of eight targets
            // 72 x 16 (the state's two zones, an eighth as wide): pixel (x, y) keeps texels 8x to
            // 8x + 7 of row y, so a worker's tile of 8 x 8 is a column of eight pixels. The same
            // pass compiled with TICK_UNPACK draws the same quads into the state texture and
            // only copies each texel from its target.
            #define TICK_MRT_W 72
            #ifdef TICK_MRT
            #define TICK_TEXELS 8
            #else
            #define TICK_TEXELS 1
            #endif
            void tick_rect(inout TriangleStream<v2f_customrendertexture> stream, float x, float y, float w, float h) {
                for (uint corner = 0; corner < 4; corner++) {
            #if defined(TICK_MRT) && !defined(TICK_UNPACK)
                    float2 p = float2((x + (corner & 1) * w) / 8, y + (corner >> 1) * h) / float2(TICK_MRT_W, STATE_ROWS);
            #else
                    float2 p = float2(x + (corner & 1) * w, y + (corner >> 1) * h) / (float2)s_dim;
            #endif
                    v2f_customrendertexture o;
                    o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
                    o.localTexcoord = float3(p, 0);
                    o.globalTexcoord = float3(p, 0);
                    o.primitiveID = 0;
                    o.direction = 0;
                    stream.Append(o);
                }
                stream.RestartStrip();
            }

            [maxvertexcount(64)]
            void tick_geom(triangle v2f_customrendertexture IN[3], uint primitive : SV_PrimitiveID, inout TriangleStream<v2f_customrendertexture> stream) {
                if (primitive != 0) return;
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                tick_rect(stream, 0, 0, 64, STATE_ROWS);
#if CORES > 1
                if (_Init) return;
                mc_geo = RAM_TEX(RAM_ADDR(MC_GEOMETRY));
                if ((mc_geo.r & 1) != 0) {
                    // being laid out anew: the whole band, whose pixels are all zeros meanwhile
                    tick_rect(stream, MC_STRIP_X, 0, 8 * MC_STRIP_ROWS, 8);
                    return;
                }
                uint tile = 0;
                for (uint k = 1; k < CORES; k++) {
                    uint tiles = mc_rows_of(mc_bits_of(k));
                    if (tiles == 0) continue;
                    mc_select(k, tile);
                    hart = k;
                    if (!mc_idle()) tick_rect(stream, MC_STRIP_X + 8 * tile, 0, 8 * tiles, 8);
                    tile += tiles;
                }
                mc_select(0, 0);
                hart = 0;
#endif
            }

            #ifdef TICK_UNPACK
            Texture2D<uint4> _TickOut0, _TickOut1, _TickOut2, _TickOut3, _TickOut4, _TickOut5, _TickOut6, _TickOut7;
            uint4 frag(v2f_customrendertexture i) : SV_Target {
                uint2 pos = i.vertex.xy;
                uint3 at = uint3(pos.x >> 3, pos.y, 0);
                uint k = pos.x & 7;
                if (k == 0) return _TickOut0.Load(at);
                if (k == 1) return _TickOut1.Load(at);
                if (k == 2) return _TickOut2.Load(at);
                if (k == 3) return _TickOut3.Load(at);
                if (k == 4) return _TickOut4.Load(at);
                if (k == 5) return _TickOut5.Load(at);
                if (k == 6) return _TickOut6.Load(at);
                return _TickOut7.Load(at);
            }
#else
#ifdef TICK_MRT
            struct tick_out {
                uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7;
            };
            #define TICK_EACH(k) for (k = 0; k < 8; k++)
            #define TICK_DONE { tick_out r; r.t0 = o[0]; r.t1 = o[1]; r.t2 = o[2]; r.t3 = o[3]; r.t4 = o[4]; r.t5 = o[5]; r.t6 = o[6]; r.t7 = o[7]; return r; }
            tick_out frag(v2f_customrendertexture i) {
#else
            #define TICK_EACH(k) k = 0;
            #define TICK_DONE return o[0];
            uint4 frag(v2f_customrendertexture i) : SV_Target {
#endif
                // o[k]: what the pixel's texels are after this pass (one, or eight with TICK_MRT)
                uint4 o[TICK_TEXELS];
                uint k;
                #define TICK_ZERO { TICK_EACH(k) o[k] = (uint4)0; TICK_DONE }
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);

#ifdef TICK_MRT
                uint2 pos = uint2(i.vertex.xy) * uint2(8, 1);
#else
                uint2 pos = i.globalTexcoord.xy * s_dim;
#endif
#if CORES > 1
                // this pixel's core, and which texel of its state it keeps
                uint2 own = pos;
                #define TICK_KEEP(k) RAM_TEX(own + uint2(k, 0))
                #define TICK_AT(k) mc_texel_of(pos + uint2(k, 0))
                mc_geo = RAM_TEX(RAM_ADDR(MC_GEOMETRY));
                mc_select(0, 0);
                hart = 0;
                if (pos.x >= MC_STRIP_X) {
                    // the workers' strip: nothing while it is laid out anew, and nothing in rows no worker has
                    if (_Init || pos.y >= 8 || (mc_geo.r & 1) != 0) TICK_ZERO
                    uint tile = (pos.x - MC_STRIP_X) >> 3;
                    hart = mc_core_at(tile);
                    if (hart == 0) TICK_ZERO
                    // which texel of the worker's state this pixel is, as a place in its rows of 64
                    uint w = ((tile - mc_tile0) << 6) + ((pos.y & 7) << 3) + (pos.x & 7);
                    pos = uint2(w & 63, w >> 6);
                }
                if (pos.y >= 64) TICK_ZERO
                if (!_Init && mc_idle()) { TICK_EACH(k) o[k] = TICK_KEEP(k); TICK_DONE }      // a worker with nothing to do
#else
                #define TICK_KEEP(k) STATE_TEX(pos + uint2(k, 0))
                #define TICK_AT(k) (pos + uint2(k, 0))
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
                        TICK_EACH(k) o[k] = STATE_TEX(TICK_AT(k));
                        TICK_DONE
                    } else {
                        cpu = cpu_init();
                    }
                } else {
                    bool has_state = false;
                    TICK_EACH(k) has_state = has_state || pixel_has_state(TICK_AT(k));
                    if (!has_state) { TICK_EACH(k) o[k] = TICK_KEEP(k); TICK_DONE }

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
                    prof_flush(TICK_AT(0));
                    #endif
                }

                /* cpu.debug_csr_val = read_csr_raw(_CheckCSR); */
                /* cpu.debug_mem_val = mem_get_word(_CheckMEM | (_CheckMEMraw ? 0 : 0x80000000)); */

                TICK_EACH(k) {
                    uint2 at = TICK_AT(k);
                    uint4 texel;
                    bool have = false;
#ifdef TICK_MRT
                    if (!_Init && !pixel_has_state(at)) { texel = TICK_KEEP(k); have = true; }
#endif
#ifndef NO_PAGING
                    if (!have && !_Init && tlb_state_texel(at, texel)) have = true;
#endif
#ifdef FPU
                    if (!have && !_Init && fp_state_texel(at, texel)) have = true;
#endif
                    if (!have) texel = encode(at);
                    o[k] = texel;
                }
                TICK_DONE
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

            // CORES > 1 (docs/multicore.md): core 0's state is the block at the left of the state
            // rows and the workers' is the strip beside it, each worker some rows of it, as the
            // geometry says (types.h); state_off is where the state of the core looked at begins
            // (mc_core is the core). RAM is read with RAM_TEX, which no core's block moves.
            #ifndef CORES
            #define CORES 1
            #endif
            #if CORES > 1
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            static uint mc_bits_now = 6;   // the write cache's size of the core looked at (types.h)
            static uint mc_size_now = 4096; // and how many texels of state it has
            static uint mc_tile0 = 0;      // and, a worker's, the first of its tiles
            static uint4 mc_geo;           // the geometry's texel, read once a pixel
            // A worker's state is a strip of whole 8 x 8 tiles in the band beside core 0's block,
            // one worker's after another's (types.h): texel w of it is in its tile w / 64. The
            // tick draws a strip as a quad of its own. What lies past a worker's last texel is
            // another worker's, and reads as the zeros it used to be (a control register a
            // worker never wrote).
            uint4 mc_state_read(uint2 pos) {
                if (mc_core == 0) return _SelfTexture2D[pos + state_off];
                uint w = pos.y * 64 + pos.x;
                if (w >= mc_size_now) return (uint4)0;
                uint t = mc_tile0 + (w >> 6), i = w & 63;
                return _SelfTexture2D[uint2(64 + (t << 3) + (i & 7), i >> 3)];
            }
            #define STATE_TEX_HART(pos, hartidx) mc_state_read(uint2(pos))
            #define STATE_TEX(pos) mc_state_read(uint2(pos))
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
                uint changed = 0, row = 0;
                mc_geo = RAM_TEX(RAM_ADDR(MC_GEOMETRY));
                for (uint h = 0; h < CORES; h++) {
                    uint rows = h == 0 ? 0 : mc_rows_of(mc_bits_of(h));
                    if (h != 0 && (rows == 0 || (mc_geo.r & 1) != 0)) continue;
                    mc_select(h, row);
                    row += rows;
                    changed |= commit_bands_changed_core();
                }
                mc_select(0, 0);
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
                mc_geo = RAM_TEX(RAM_ADDR(MC_GEOMETRY));
                if (pos.y < 64) {
                    // a core's own state, as with one core
                    uint hb = 0;
                    uint4 own = RAM_TEX(pos);
                    mc_select(0, 0);
                    if (pos.x >= MC_STRIP_X) {
                        if (pos.y >= 8 || (mc_geo.r & 1) != 0) return (uint4)0;
                        uint tile = (pos.x - MC_STRIP_X) >> 3;
                        hb = mc_core_at(tile);
                        if (hb == 0) return (uint4)0;
                        uint w = ((tile - mc_tile0) << 6) + ((pos.y & 7) << 3) + (pos.x & 7);
                        pos = uint2(w & 63, w >> 6);
                    }
                    pos = mc_texel_of(pos);
                    decode_for_commit();
                    result = commit(pos, own);
                    // A worker's one store that found its cache full is in its state until its next
                    // pass rewrites it: gone now, or the commits of a worker asleep would store it again.
                    if (hb != 0 && pos.x == 8 && pos.y == 0) result.r = 0xffffffff;
                } else {
                    // RAM: every core's writes, the highest core's last
                    result = RAM_TEX(pos);
                    uint row = 0;
                    for (uint h = 0; h < CORES; h++) {
                        uint rows = h == 0 ? 0 : mc_rows_of(mc_bits_of(h));
                        if (h != 0 && (rows == 0 || (mc_geo.r & 1) != 0)) continue;
                        mc_select(h, row);
                        row += rows;
                        if (h != 0 && STATE_TEX(uint2(41, 0)).r == 0) continue;   // a worker that stored nothing
                        decode_for_commit();
                        result = commit(pos, result);
                    }
                    mc_select(0, 0);
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
