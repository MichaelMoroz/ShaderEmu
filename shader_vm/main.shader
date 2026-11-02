Shader "Nix/shader_vm"
{
    Properties
    {
        [ToggleUI] _Init ("Init", float) = 0
        _Ticks ("Ticks per Frame", Int) = 1024

        _UdonUARTInChar ("UART input Udon side", Int) = 0
        _UdonUARTInTag ("UART input unique tag", Int) = 0

        _PlayerID ("Player ID", Int) = -1
        _RTC0 ("RTC0", Int) = 0
        _RTC1 ("RTC1", Int) = 0

        _Data_RAM ("The start image, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}



        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}

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
            // (RAM written in place; the GPU device and the sound card are gpu.shader and sound.shader)
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
            #include "crt.cginc"
            #include "UnityCG.cginc"

            float _Init;
            uniform uint _CommitAlways;   // COMMIT_BANDS: bands 0 to 31 drawn every commit (the control words' 28)

            Texture2D<float4> _Data_RAM;   // the start image: as the ROM

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
            static uint2 mc_slot = uint2(0, 0);   // and where its slot is
            static uint4 mc_geo;           // the geometry's texel, read once a pixel
            // A core's state is its slot of 64 x 8 texels (types.h). What lies past a worker's last
            // texel reads as zeros (a control register a worker never wrote).
            uint4 mc_state_read(uint2 pos) {
                if (mc_core == 0) return STATE_SRC[pos + state_off];
                uint w = pos.y * 64 + pos.x;
                if (w >= mc_size_now) return (uint4)0;
                return STATE_SRC[mc_slot + uint2(((w >> 6) << 3) + (w & 7), (w >> 3) & 7)];
            }
            #define STATE_TEX_HART(pos, hartidx) mc_state_read(uint2(pos))
            #define STATE_TEX(pos) mc_state_read(uint2(pos))
            #else
            #define STATE_TEX_HART(pos, hartidx) (STATE_SRC[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (STATE_SRC[pos])
            #endif

            static uint2 s_dim;

            #include "helpers.cginc"
            #include "src/types.h" // includes fb.h

#ifdef GPU_DEVICE
            // The GPU device's picture, for copying into the RAM framebuffer (docs/machine/gpu.md).
            Texture2D<float4> _GpuTarget;
            #define GPU_STATE _SelfTexture2D
            #ifndef RAM_INPLACE
            #define GPU_WRITEBACK
            #endif
            #include "src/gpu.h"
#endif

            #include "src/zap.h"
                        #include "src/ram.h"
            #ifdef COMMIT_GATHER
            ram_point commit_vert(uint id : SV_VertexID) { return ram_point_vert(id); }
            uint4 frag(ram_v2f i) : SV_Target { return ram_gather_texel(i); }
            #elif defined(COMMIT_RAM)
            ram_point commit_vert(uint id : SV_VertexID) { return ram_point_vert(id); }
            uint4 frag(ram_v2f i) : SV_Target { return ram_commit_texel(i); }
            #else
            // the state commit: one quad of the 64 state rows
            ram_v2f commit_vert(uint id : SV_VertexID) {
                static const float2 corners[6] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}, {0, 0}, {1, 1}};
                float2 p = float2(corners[id % 6].x, corners[id % 6].y * 64.0 / STATE_TEX_HEIGHT);
                ram_v2f o;
                o.vertex = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.5, 1.0);
                o.value = 0;
                o.info = 0;
                return o;
            }
            uint4 frag(ram_v2f i) : SV_Target { return ram_state_texel((uint2)i.vertex.xy); }
            #endif
            ENDCG
        }
    }
}
