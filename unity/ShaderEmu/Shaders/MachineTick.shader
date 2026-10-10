Shader "ShaderEmu/MachineTick"
{
    // The machine's CPUTick pass (experiments/rvc_opt/main.shader's with TICK_MRT), drawn by a
    // camera of its own that EmuMachine.cs renders once a round: a pass with several targets
    // is a camera's in VRChat. A pixel is eight texels of state, one in each of eight targets
    // (docs/multicore.md, "Eight texels a pixel"). It includes none of the GPU device's sources.
    Properties
    {
        _SelfTexture2D ("State before the pass", 2D) = "black" {}
        _TickState ("CPU state area after the tick", 2D) = "black" {}
        [ToggleUI] _Init ("Init", Int) = 0
        [ToggleUI] _InitRaw ("Init directly from _Data_RAM_RAW", Int) = 0
        _Ticks ("Ticks per Frame", Int) = 2048

        _UartInLo ("UART input, characters 1-2", Int) = 0
        _UartInHi ("UART input, characters 3-4", Int) = 0
        _UdonUARTInTag ("UART input unique tag", Int) = 0

        _PlayerID ("Player ID", Int) = 0
        _Rtc0Lo ("RTC0 low half", Int) = 0
        _Rtc0Hi ("RTC0 high half", Int) = 0
        _Rtc1Lo ("RTC1 low half", Int) = 0
        _Rtc1Hi ("RTC1 high half", Int) = 0
        _DoTick ("DEBUG: Perform single step", Int) = 0

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

        _GpuTarget ("The GPU's colour target", 2D) = "black" {}

        _InputPointer ("Pointer x, y and panel size", Vector) = (0, 0, 0, 0)
        _InputButtons ("Pointer buttons", Int) = 0
        _InputKeySeq ("Key events delivered before this frame", Int) = 0
        _InputKeyCount ("Key events this frame", Int) = 0
        _InputKeyCode0 ("Key event 0: code, +65536 while pressed", Int) = 0
        _InputKeyCode1 ("Key event 1", Int) = 0
        _InputKeyCode2 ("Key event 2", Int) = 0
        _InputKeyCode3 ("Key event 3", Int) = 0
        _HostMsLo ("Host clock in ms, low half", Int) = 0
        _HostMsHi ("Host clock in ms, high half", Int) = 0
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
            #pragma vertex blit_vert
            #pragma geometry tick_geom
            #pragma fragment frag
            #pragma editor_sync_compilation
            // One permutation only: the full machine, started in the kernel with no firmware
            // (docs/boot.md). Images that need machine-mode start-up do not boot on it.
            #define SBI_HLE
            // Built by fxc2 (docs/fxc2.md): the write cache and the TLB as locals, MULH in one
            // instruction. Unity preprocesses the source itself, so the compiler cannot say
            // what it is; a stock editor (FXC) needs this line taken out.
            #define __FXC2__
            // Single-precision float instructions (docs/fpu.md), as the harness's full machine has.
            #define FPU
            // The tick says which 4 MB bands of RAM it wrote: Machine.shader draws only those.
            #define COMMIT_BANDS

            #define PASS_TICK

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"

            uniform uint _Init, _InitRaw;
            uniform uint _Ticks;
            uniform uint _UartInLo, _UartInHi, _UdonUARTInTag;
            uniform uint _PlayerID;
            uniform uint _Rtc0Lo, _Rtc0Hi, _Rtc1Lo, _Rtc1Hi;
            uniform uint _DoTick;
            static uint _UdonUARTInChar, _RTC0, _RTC1;

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

            // Core 0's state is the block at the left of the state rows and the workers' is the
            // band beside it, each worker a strip of whole 8 x 8 tiles of it, as the geometry
            // says (src/types.cginc, docs/multicore.md); mc_core is the core looked at. RAM is
            // read with RAM_TEX, which no core's block moves. As experiments/rvc_opt/main.shader.
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            static uint mc_bits_now = 6;    // the write cache's size of the core looked at
            static uint mc_size_now = 4096; // and how many texels of state it has
            static uint mc_tile0 = 0;       // and, a worker's, the first of its tiles
            static uint4 mc_geo;            // the geometry's texel, read once a pixel
            static uint4 mc_geo1, mc_geo2;  // and the two of workers 25 and up (MC_GEOMETRY_MORE)
            // Texel w of a worker's state is in its tile w / 64; what lies past its last texel is
            // another worker's, and reads as zeros.
            uint4 mc_state_read(uint2 pos) {
                if (mc_core == 0) return _SelfTexture2D[pos + state_off];
                uint w = pos.y * 64 + pos.x;
                if (w >= mc_size_now) return (uint4)0;
                uint t = mc_tile0 + (w >> 6), i = w & 63;
                return _SelfTexture2D[uint2(64 + (t << 3) + (i & 7), i >> 3)];
            }
            #define STATE_TEX_HART(pos, hartidx) mc_state_read(uint2(pos))
            #define STATE_TEX(pos) mc_state_read(uint2(pos))
            #define RAM_TEX(pos) (_SelfTexture2D[pos])

            static uint2 s_dim;
            static uint2 m_dim;

            #include "helpers.cginc"
            #include "src/prof.cginc"

            #include "src/types.cginc"
            #include "src/mc.cginc"
            #include "src/ins.cginc"
            #include "src/uart.cginc"
            #include "src/emu.cginc" // includes mmu.h, csr.h, mem.h, trap.h
            #include "src/cpu.cginc"

            // What the tick draws, whatever the mesh is: a quad for core 0's rectangle, and one for
            // each worker with something to run, in the worker's own block of columns (a column
            // is eight texels across: a tile). What is not drawn costs no pixel, and Machine.shader's
            // Unpack takes it from the state as it was.
            void tick_rect(inout TriangleStream<blit_v2f> stream, float x, float y, float w, float h) {
                for (uint corner = 0; corner < 4; corner++) {
                    float2 p = float2((x + (corner & 1) * w) / 8, y + (corner >> 1) * h) / float2(TICK_MRT_W, TICK_STATE_ROWS);
                    blit_v2f o;
                    // clip space as the card has it: the texture's top left is (-1, 1)
                    o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.5, 1.0);
                    stream.Append(o);
                }
                stream.RestartStrip();
            }

            [maxvertexcount(64)]
            void tick_geom(triangle blit_v2f corners[3], uint which : SV_PrimitiveID, inout TriangleStream<blit_v2f> stream) {
                // only the tick's own camera draws it (its targets' size says which that is)
                // (a primitive is 64 vertices at most: the mesh has one for every sixteen cores)
                if (which >= (CORES + 15) / 16 || (uint)_ScreenParams.x != TICK_MRT_W || (uint)_ScreenParams.y != TICK_STATE_ROWS) return;
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                if (which == 0) tick_rect(stream, 0, 0, 64, STATE_ROWS);
                if (_Init) return;
                MC_GEO_READ
                if ((mc_geo.r & 1) != 0) return;   // being laid out anew: the band is zeros meanwhile (Unpack)
                uint tile = 0, col = 0;
                for (uint k = 1; k < CORES; k++) {
                    uint tiles = mc_rows_of(mc_bits_of(k));
                    if (tiles == 0) continue;
                    if (k / 16 == which) {
                        mc_select(k, tile);
                        hart = k;
                        if (!mc_idle()) tick_rect(stream, MC_STRIP_X + 8 * (TICK_GAP + col), 0, 8 * mc_cols_of(tiles), 8);
                    }
                    tile += tiles;
                    col += mc_cols_of(tiles);
                }
                mc_select(0, 0);
                hart = 0;
            }

            struct tick_out {
                uint4 t0 : SV_Target0; uint4 t1 : SV_Target1; uint4 t2 : SV_Target2; uint4 t3 : SV_Target3;
                uint4 t4 : SV_Target4; uint4 t5 : SV_Target5; uint4 t6 : SV_Target6; uint4 t7 : SV_Target7;
            };
            // o[k]: what the pixel's eight texels are after this pass
            #define TICK_EACH(k) for (k = 0; k < 8; k++)
            #define TICK_DONE { tick_out r; r.t0 = o[0]; r.t1 = o[1]; r.t2 = o[2]; r.t3 = o[3]; r.t4 = o[4]; r.t5 = o[5]; r.t6 = o[6]; r.t7 = o[7]; return r; }
            #define TICK_ZERO { TICK_EACH(k) o[k] = (uint4)0; TICK_DONE }
            #define TICK_KEEP(k) RAM_TEX(own + uint2(k, 0))
            #define TICK_AT(k) mc_texel_of(pos + uint2(k, 0))

            tick_out frag(blit_v2f i) {
                uint4 o[8];
                uint k;
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);

                uint2 pos = (uint2)i.vertex.xy * uint2(8, 1);
                // this pixel's core, and which texels of its state it keeps
                uint2 own = pos;
                MC_GEO_READ
                mc_select(0, 0);
                hart = 0;
                if (pos.x >= MC_STRIP_X) {
                    // the workers' columns: nothing while they are laid out anew, and nothing in columns no worker has
                    if (_Init || pos.y >= 8 || (mc_geo.r & 1) != 0) TICK_ZERO
                    uint tile = (pos.x - MC_STRIP_X) >> 3;
                    // the column: whose block it is in, and which of the worker's tiles it is
                    if (tile < TICK_GAP) TICK_ZERO
                    tile -= TICK_GAP;
                    hart = mc_core_find(tile, true);
                    if (hart == 0) TICK_ZERO
                    tile = mc_tile0 + (tile - mc_col0);
                    if (tile - mc_tile0 >= mc_rows_of(mc_bits_now)) TICK_ZERO
                    own = uint2(MC_STRIP_X + 8 * tile, pos.y);
                    // which texel of the worker's state this pixel begins at, as a place in its rows of 64
                    uint w = ((tile - mc_tile0) << 6) + ((pos.y & 7) << 3);
                    pos = uint2(w & 63, w >> 6);
                }
                if (pos.y >= 64) TICK_ZERO
                if (!_Init && mc_idle()) { TICK_EACH(k) o[k] = TICK_KEEP(k); TICK_DONE }      // a worker with nothing to do
#ifdef L1_LOCAL
                uint4 l1_cache[L1_DATA_N];
#if L1_WAYS == 4
                uint4 l1_tag[L1_BUCKETS];
#endif
                uint tlb2_tag[3 * TLB2_N];
                uint tlb2_pg[3 * TLB2_N];
#endif

                _UdonUARTInChar = _UartInLo | (_UartInHi << 16);
                _RTC0 = _Rtc0Lo | (_Rtc0Hi << 16);
                _RTC1 = _Rtc1Lo | (_Rtc1Hi << 16);
                uint ticks = max(_Ticks, 2);

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
                    mc_enter();
                    xreg_load();
#ifdef FPU
                    fp_state_load();
#endif
#ifndef NO_PAGING
                    tlb_state_load();
#endif

                    uint n = 0;
                    [loop]
                    while (n < ticks && !cpu.stall) {
                        // as many fast ticks in a row as possible, then one general tick
                        n += fast_run(ticks - n);
                        if (n < ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            n++;
                        }
                    }
                    xreg_store();
                }

                TICK_EACH(k) {
                    uint2 at = TICK_AT(k);
                    uint4 texel;
                    bool have = false;
                    if (!_Init && !pixel_has_state(at)) { texel = TICK_KEEP(k); have = true; }
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
            ENDCG
        }
    }
}
