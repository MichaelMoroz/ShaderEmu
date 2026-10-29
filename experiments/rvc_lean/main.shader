Shader "Nix/rvc_lean"
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

        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}

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

            HLSLPROGRAM
            // (rvc_lean: its own tick; the commit, the GPU device and the sound card are rvc_opt's, RAM written in place)
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry tick_geom
            #pragma fragment frag
            #define LANE_FIELD   // the lanes' field (docs/machine/lanes.md): the host draws it where it sees this
            #include "tick.hlsl"
            ENDHLSL
        }

        Pass
        {
            Name "Commit"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex commit_vert
            #pragma fragment frag

            #define PASS_COMMIT
            #define LANE_FIELD   // (its points: src/ram.h)

            // Cache buster: 31

            // custom crt include
            #include "../rvc_opt/crt.cginc"
            #include "UnityCG.cginc"

            float _Init, _InitRaw;
            uniform uint _CommitAlways;   // COMMIT_BANDS: bands 0 to 31 drawn every commit (the control words' 28)

            Texture2D<float4> _Data_RAM_R;
            Texture2D<float4> _Data_RAM_G;
            Texture2D<float4> _Data_RAM_B;
            Texture2D<float4> _Data_RAM_A;
            Texture2D<float4> _Data_RAM_RAW;

            Texture2D<float4> _Data_ROM;   // the ROM: a 16-byte texel a pixel, read by its bits (asuint)

            // CORES > 1 (docs/machine/multicore.md): core 0's state is the block at the left of the state
            // rows and the workers' is the strip beside it, each worker some rows of it, as the
            // geometry says (types.h); state_off is where the state of the core looked at begins
            // (mc_core is the core). RAM is read with RAM_TEX, which no core's block moves.
            #ifndef CORES
            #define CORES 1
            #endif
            #ifdef RAM_INPLACE
            // RAM in one texture (src/ram.h): the state after the tick is a texture of its own,
            // and RAM is read only by the gather; the other two passes see the control words.
            Texture2D<uint4> _TickState, _Stage;
            #define STATE_SRC _TickState
            #ifdef COMMIT_GATHER
            Texture2D<uint4> _Ram;
            #define RAM_TEX(pos) (_Ram[uint2(pos)])
            #else
            uint4 ram_stage_read(uint2 pos);
            #define RAM_TEX(pos) ram_stage_read(uint2(pos))
            #endif
            #else
            #define STATE_SRC _SelfTexture2D
            #define RAM_TEX(pos) (_SelfTexture2D[pos])
            #endif
            #if CORES > 1
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            static uint mc_bits_now = 6;   // the write cache's size of the core looked at (types.h)
            static uint mc_size_now = 4096; // and how many texels of state it has
            static uint mc_tile0 = 0;      // and, a worker's, the first of its tiles
            static uint4 mc_geo;           // the geometry's texel, read once a pixel
            // A worker's state is a strip of whole 8 x 8 tiles in the bands beside core 0's block,
            // 248 tiles to a band (types.h): texel w of it is in its tile w / 64. The
            // tick draws a strip as a quad of its own. What lies past a worker's last texel is
            // another worker's, and reads as the zeros it used to be (a control register a
            // worker never wrote).
            uint4 mc_state_read(uint2 pos) {
                if (mc_core == 0) return STATE_SRC[pos + state_off];
                uint w = pos.y * 64 + pos.x;
                if (w >= mc_size_now) return (uint4)0;
                uint t = mc_tile0 + (w >> 6), i = w & 63;
                return STATE_SRC[uint2(64 + ((t % 248) << 3) + (i & 7), ((t / 248) << 3) + (i >> 3))];
            }
            #define STATE_TEX_HART(pos, hartidx) mc_state_read(uint2(pos))
            #define STATE_TEX(pos) mc_state_read(uint2(pos))
            #else
            #define STATE_TEX_HART(pos, hartidx) (STATE_SRC[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (STATE_SRC[pos])
            #endif

            static uint2 s_dim;
            static uint2 m_dim;

            #include "../rvc_opt/helpers.cginc"
            #include "../rvc_opt/src/types.h" // includes fb.h

#ifdef GPU_DEVICE
            // The GPU device's picture, for copying into the RAM framebuffer (docs/machine/gpu.md).
            Texture2D<float4> _GpuTarget;
            #define GPU_STATE _SelfTexture2D
            #ifndef RAM_INPLACE
            #define GPU_WRITEBACK
            #endif
            #include "../rvc_opt/src/gpu.h"
#endif

            #include "../rvc_opt/src/zap.h"
            #ifdef RAM_INPLACE
            #include "../rvc_opt/src/ram.h"
            #ifdef COMMIT_GATHER
            ram_point commit_vert(uint id : SV_VertexID) { return ram_point_vert(id); }
            uint4 frag(ram_v2f i) : SV_Target { return ram_gather_texel(i); }
            #elif defined(COMMIT_RAM)
            ram_point commit_vert(uint id : SV_VertexID) { return ram_point_vert(id); }
            uint4 frag(ram_v2f i) : SV_Target { return ram_commit_texel(i); }
            #else
            // the state commit: one quad of the 64 state rows, as wide as the cores' block (layout.h)
            ram_v2f commit_vert(uint id : SV_VertexID) {
                static const float2 corners[6] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}};
                float2 p = float2(corners[id % 6].x * STATE_BLOCK_W / RAM_W, corners[id % 6].y * RAM_ROW / STATE_TEX_HEIGHT);
                ram_v2f o;
                o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.5, 1.0);
                o.value = 0;
                o.info = 0;
                return o;
            }
            uint4 frag(ram_v2f i) : SV_Target { return ram_state_texel((uint2)i.vertex.xy); }
            #endif
            #else

#ifdef COMMIT_BANDS
#if RAM_TILE_BITS != 0
#error COMMIT_BANDS needs RAM in row order
#endif
            // RAM in 4 MB bands (RAM_BAND_ROWS rows), as ram_bands (types.h): those this commit changes. The other
            // buffer holds the state of two commits ago, so a band neither this commit nor the
            // last one changes is already right there and is not drawn. The GPU's control words'
            // band is drawn every commit (_CommitAlways), not through these bits.
#if CORES > 1
            uint commit_bands_changed_core();
            // every core's writes
            uint commit_bands_changed() {
                uint changed = 0;
                MC_GEO_READ
#ifdef COMMIT_SCATTER
                // (what the cores stored is points: the bands are the same whoever is asked)
                uint most = 1;
#else
                uint most = mc_count + 1;
#endif
                for (uint h = 0; h < most; h++) {
                    mc_select(h);
                    changed |= commit_bands_changed_core();
                }
                mc_select(0);
                return changed;
            }
            uint commit_bands_changed_core() {
#else
            uint commit_bands_changed() {
#endif
                uint stalled = STATE_TEX_HART(uint2(28, 0), 0).r;
                if (_Init) return 0xffffffff;
#ifdef COMMIT_SCATTER
                uint changed = 0;   // what the cores stored is drawn as points (COMMIT_POINTS)
                if (false) {
#else
                uint changed = STATE_TEX_HART(uint2(41, 0), 0).g;   // the tick's writes
                if (stalled == STALL_MEMOP_COPY || stalled == STALL_MEMOP_FILL) {
#endif
                    // a parallel copy or fill: the bands from its destination to its end
                    uint4 op = STATE_TEX_HART(uint2(38, 0), 0);
#ifdef CSR_FIELDS
                    op.g = STATE_TEX_HART(uint2(37, 0), 0).a & 0x7fffffff;   // (no translation: types.h)
#endif
                    if (op.b != 0) changed |= ram_bands((op.g & 0x7fffffff) >> 22, min(((op.g & 0x7fffffff) + op.b - 1) >> 22, (uint)(RAM_MAX - 1) >> 22));
                }
#if !defined(COMMIT_SCATTER)
                // a fill or copy the pass left to the commit (LAZY_MEMOP): its destination's bands
                // (the first: its word and destination; the second: the same of the other, types.h)
                uint2 lazy = uint2(STATE_TEX_HART(uint2(40, 0), 0).a, STATE_TEX_HART(uint2(38, 0), 0).a);
                uint2 lazy_at = uint2(STATE_TEX_HART(uint2(39, 0), 0).r, STATE_TEX_HART(uint2(38, 0), 0).g);
                for (uint which = 0; which < 2; which++) {
                    if (lazy[which] == 0) continue;
                    uint lo2 = (lazy_at[which] & 0x7fffffff) >> 22, hi2 = min(((lazy_at[which] & 0x7fffffff) + ((lazy[which] >> 2) << 4) - 1) >> 22, (uint)(RAM_MAX - 1) >> 22);
                    changed |= ram_bands(lo2, hi2);
                }
#endif
#ifdef GPU_DEVICE
                changed |= gpu_copy_bands();
#endif
                changed |= zap_bands();   // (the host's hand in RAM, for this commit only)
                return changed;
            }
#endif

            // One quad for the whole texture, or with COMMIT_BANDS (1 + RAM's rows / RAM_BAND_ROWS quads) the
            // state rows and each band of RAM that has to be drawn.
            // COMMIT_POINTS (the D3D11 harness with the tick in eight targets, docs/machine/multicore.md):
            // what the cores stored is drawn as points, each made the rectangle it stands for
            // by commit_geom: a texel for each entry of a bucket of a write cache and for the store
            // a full cache had no room for, the rows of its destination for a copy or a fill. Point
            // n is core 0's bucket n (256 places, 112 used, then its last store and its copy), and
            // from 258 on 130 a worker (128 places). Where the stores are is read from the tick's
            // targets, as the tick left them: a worker that did not run has its last pass's
            // there, whose texels are then written again as they are, which is harmless. The
            // bands the commit draws whole are then only those something else wrote.
#ifdef COMMIT_POINTS
            Texture2D<uint4> _TickOut0, _TickOut1, _TickOut2, _TickOut3, _TickOut4, _TickOut5, _TickOut6, _TickOut7;
            uint4 tick_out(uint k, uint2 at) {
                uint3 p = uint3(at, 0);
                if (k == 0) return _TickOut0.Load(p);
                if (k == 1) return _TickOut1.Load(p);
                if (k == 2) return _TickOut2.Load(p);
                if (k == 3) return _TickOut3.Load(p);
                if (k == 4) return _TickOut4.Load(p);
                if (k == 5) return _TickOut5.Load(p);
                if (k == 6) return _TickOut6.Load(p);
                return _TickOut7.Load(p);
            }
            // texel w of a core's state as the tick left it
            uint4 tick_state(uint core, uint w) {
                if (core == 0) return tick_out(w & 7, uint2((w & 63) >> 3, w >> 6));
                return tick_out(w & 7, mc_block_of(core) + uint2(w >> 6, (w & 63) >> 3));
            }
            static uint commit_core = 0;   // whose store the quads are: the pixel asks that core alone
            void commit_quad(inout TriangleStream<v2f_customrendertexture> stream, uint x, uint y, uint w, uint h) {
                for (uint corner = 0; corner < 4; corner++) {
                    float2 p = float2(x + (corner & 1) * w, y + (corner >> 1) * h) / (float2)s_dim;
                    v2f_customrendertexture o;
                    o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
                    o.localTexcoord = float3(p, 0);
                    o.globalTexcoord = float3(p, 0);
                    o.primitiveID = commit_core;
                    o.direction = 0;
                    stream.Append(o);
                }
                stream.RestartStrip();
            }
            #define COMMIT_RAM_TEXELS RAM_TEXELS
            // the texels from t0 to t1, row by row
            void commit_rows(inout TriangleStream<v2f_customrendertexture> stream, uint t0, uint t1) {
                if (t0 > t1) return;
                uint y0 = t0 / RAM_W, y1 = t1 / RAM_W, x0 = t0 % RAM_W, x1 = t1 % RAM_W;
                if (y0 == y1) {
                    commit_quad(stream, x0, RAM_ROW + y0, x1 - x0 + 1, 1);
                } else {
                    commit_quad(stream, x0, RAM_ROW + y0, RAM_W - x0, 1);
                    commit_quad(stream, 0, RAM_ROW + y1, x1 + 1, 1);
                    if (y1 > y0 + 1) commit_quad(stream, 0, RAM_ROW + y0 + 1, RAM_W, y1 - y0 - 1);
                }
            }
            [maxvertexcount(12)]
            void commit_geom(point v2f_customrendertexture IN[1], inout TriangleStream<v2f_customrendertexture> stream) {
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                uint id = IN[0].primitiveID, core = 0, e = id, bits = L1_TABLE_BITS;
#if CORES > 1
                if (id >= 258) {
                    core = 1 + (id - 258) / 130;
                    e = (id - 258) % 130;
                    if (core >= CORES) return;
                    MC_GEO_READ
                    bits = mc_bits_of(core);
                    if (bits == 0) return;
                }
#else
                if (id >= 258) return;
#endif
                commit_core = core;
                uint last = core == 0 ? 256 : 128;
                if (e < last) {
                    // a bucket of the cache: an entry's tag is the texel's number and one
                    if (e >= L1_BUCKETS_OF(bits)) return;
                    uint4 tags = tick_state(core, L1_STATE_AT + e * L1_STRIDE);
                    for (uint n = 0; n < 3; n++) {
                        uint tag = idx_uint4(tags, n);
                        if (tag != 0 && tag - 1 < COMMIT_RAM_TEXELS) commit_quad(stream, (tag - 1) % RAM_W, RAM_ROW + (tag - 1) / RAM_W, 1, 1);
                    }
                } else if (e == last) {
                    // the store a full cache had no room for
                    uint addr = tick_state(core, 8).r;
                    if ((addr >> 4) < COMMIT_RAM_TEXELS) commit_quad(stream, (addr >> 4) % RAM_W, RAM_ROW + (addr >> 4) / RAM_W, 1, 1);
                } else {
                    // a copy or a fill: the texels from its destination to its end, row by row
                    // (the ones the pass left to the commit, two at most and three rectangles between
                    // them, which the tick sees to; or else the one that ended it)
                    uint stalled = tick_state(core, 28).r, lazy = tick_state(core, 40).a;
                    uint4 op = tick_state(core, 38);
#ifdef CSR_FIELDS
                    op.g = tick_state(core, 37).a & 0x7fffffff;   // (no translation: types.h)
#endif
                    if (lazy != 0) {
                        uint first = tick_state(core, 39).r >> 4;
                        commit_rows(stream, first, min(first + (lazy >> 2) - 1, COMMIT_RAM_TEXELS - 1));
                        if (op.a != 0) commit_rows(stream, op.g >> 4, min((op.g >> 4) + (op.a >> 2) - 1, COMMIT_RAM_TEXELS - 1));
                        return;
                    }
                    if ((stalled != STALL_MEMOP_COPY && stalled != STALL_MEMOP_FILL) || op.b == 0) return;
                    commit_rows(stream, op.g >> 4, min((op.g + op.b - 1) >> 4, COMMIT_RAM_TEXELS - 1));
                }
            }
#endif

            v2f_customrendertexture commit_vert(appdata_customrendertexture IN) {
#ifdef COMMIT_POINTS
                v2f_customrendertexture OUT;
                OUT.vertex = float4(0, 0, 0, 1);
                OUT.localTexcoord = 0;
                OUT.globalTexcoord = 0;
                OUT.primitiveID = IN.vertexID;
                OUT.direction = 0;
                return OUT;
#elif defined(COMMIT_BANDS)
                static const float2 corners[6] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}};
                uint quad = IN.vertexID / 6;
                float2 corner = corners[IN.vertexID % 6];
                float top = quad == 0 ? 0.0 : RAM_ROW + (quad - 1) * RAM_BAND_ROWS, rows = quad == 0 ? RAM_ROW : RAM_BAND_ROWS;
                uint drawn = commit_bands_changed() | STATE_TEX_HART(uint2(41, 0), 0).b, band = quad - 1;
                bool on = quad == 0 || ((drawn >> (band & 31)) & 1) != 0 || (band < 32 && ((_CommitAlways >> band) & 1) != 0);
                // (the state rows as wide as the cores' block: the rest of them is no one's)
                float2 uv = float2(corner.x * (quad == 0 ? (float)STATE_BLOCK_W / RAM_W : 1.0), (top + corner.y * rows) / _CustomRenderTextureInfo.y);
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
                _Data_ROM.GetDimensions(m_dim.x, m_dim.y);
                uint2 pos = i.globalTexcoord.xy * s_dim;
#ifdef COMMIT_COPY
                // the same points once more, into the buffer the commit did not draw into: its
                // texels there are what the commit made them
                return _SelfTexture2D[pos];
#endif

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
                MC_GEO_READ
                if (pos.y < RAM_ROW) {
                    // a core's own state, as with one core (and no one's beside the cores' block)
                    if (pos.x >= STATE_BLOCK_W) return (uint4)0;
                    uint hb = 0;
                    uint4 own = RAM_TEX(pos);
                    mc_select(0);
                    if (pos.x >= MC_STRIP_X) {
                        uint tile = ((pos.x - MC_STRIP_X) >> 3) + MC_STRIP_TILES * (pos.y >> 3);
                        hb = mc_core_at(tile);
                        if (hb == 0) return (uint4)0;
                        uint w = ((tile - mc_tile0) << 6) + ((pos.y & 7) << 3) + (pos.x & 7);
                        pos = uint2(w & 63, w >> 6);
                    }
                    pos = mc_texel_of(pos);
                    // (of a worker's state the commit changes three texels: the rest is as it is)
                    if (hb != 0 && (pos.y != 0 || (pos.x != 8 && pos.x != 28 && pos.x != 41))) return own;
                    decode_for_commit();
                    result = commit(pos, own);
                    // A worker's one store that found its cache full is in its state until its next
                    // pass rewrites it: gone now, or the commits of a worker asleep would store it again.
                    if (hb != 0 && pos.x == 8 && pos.y == 0) result.r = 0xffffffff;
                } else {
                    // RAM: every core's writes, the highest core's last
                    result = RAM_TEX(pos);
#if defined(COMMIT_POINTS)
                    // a point: the store of one core (two may not store to one texel in a pass). A
                    // pixel that asked every core took as long as hundreds of the tick's instructions.
                    uint first = min(i.primitiveID, mc_count), most = first + 1;
#elif defined(COMMIT_SCATTER)
                    // (a band drawn whole: what a core stored there is a point, drawn after this)
                    uint first = 0, most = _Init ? 1 : 0;
#else
                    uint first = 0, most = mc_count + 1;
#endif
                    bool stored_here = false;
                    for (uint h = first; h < most; h++) {
                        mc_select(h);
                        if (h != 0) {
                            // A worker that stored nothing, or nothing here: every address it
                            // stored to is in the word it keeps of them all or-ed together, so a
                            // texel whose address has a bit that word lacks is not its. (A copy
                            // or a fill is not in that word.)
                            uint stored = STATE_TEX(uint2(41, 0)).r;
                            if (stored == 0) continue;
                            uint stalled = STATE_TEX(uint2(28, 0)).r;
                            if (((RAM_LIN(pos.x, pos.y - RAM_ROW) << 4) & ~stored) != 0 && stalled != STALL_MEMOP_COPY && stalled != STALL_MEMOP_FILL) continue;
                        }
                        decode_for_commit();
                        result = commit(pos, result);
                        stored_here = true;
                    }
                    mc_select(0);
#if !defined(COMMIT_POINTS)
                    // (in the pass that draws bands whole: a core's store to the same texel is a point drawn after)
                    result = zap(pos, result);
#endif
#if defined(COMMIT_POINTS)
                    // A worker that sat the pass out has its last pass's tags in the targets still:
                    // its point is of no store, and must not put the texel as it was over what
                    // another core's point has just drawn there.
                    if (!stored_here) discard;
#endif
                }
#else
                decode_for_commit();
                uint4 result = commit(pos, STATE_TEX_HART(pos, 0));
                if (pos.y >= RAM_ROW) result = zap(pos, result);
#endif
#ifdef COMMIT_BANDS
                if (pos.x == 41 && pos.y == 0) result.b = commit_bands_changed();   // for the next commit
#endif
                return result;
            }
            #endif
            ENDCG
        }
    }
}
