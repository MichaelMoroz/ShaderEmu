Shader "ShaderEmu/Machine"
{
    // The machine's Commit pass (experiments/rvc_opt/main.shader's) and the GPU device's control
    // pass, drawn by EmuMachine.cs with VRCGraphics.Blit between two state textures. The CPUTick
    // pass is MachineTick.shader: a file of its own, so that nothing changed here or in the GPU's
    // sources makes Unity compile it again (four minutes).
    // A material's numbers are floats, exact to 24 bits, so 32-bit values arrive as two halves.
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
        _HostFlags ("Host flags (bit 0: desktop at boot; bits 8-15, 16-23: largest screen / 16)", Int) = 1
        _FetchDeliver ("An answer to the guest's request arrives this frame", Int) = 0
        _FetchSeq ("The request it answers", Int) = 0
        _FetchLength ("Its length in bytes", Int) = 0
        _FetchStatus ("Its status (200: fine)", Int) = 0
        _HostData ("Its bytes", 2D) = "black" {}
        _HostImage ("Or a picture, with _FetchDeliver 2", 2D) = "black" {}
        _FetchInfo ("A picture's width | height << 16", Int) = 0
        _FetchW ("or its width", Int) = 0
        _FetchH ("and its height", Int) = 0
        _SoundCursorLo ("Sound: the sample the host's ring starts at, low half", Int) = 0
        _SoundCursorHi ("and high half", Int) = 0
        _SoundMixed ("Sound: the host mixed before this control pass", Int) = 0
        _SoundRate ("Sound: output samples a second", Int) = 48000
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
            Name "Commit"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry blit_bands_geom
            #pragma fragment frag
            #pragma editor_sync_compilation

            #define PASS_COMMIT
            #define GPU_DEVICE
            #define BLIT_BANDS

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"

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

            // A core's state area as the tick left it (state_off is where the core's block
            // starts: docs/multicore.md); everything else as it was.
            #define MC_BLOCK(k) uint2((k) * CORE_PITCH, 0)
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            #define STATE_TEX_HART(pos, hartidx) (state_after_tick(uint2(pos) + state_off))
            #define STATE_TEX(pos) (state_after_tick(uint2(pos) + state_off))
            #define RAM_TEX(pos) (_SelfTexture2D[pos])   // the tick does not write RAM

            static uint2 s_dim;
            static uint2 m_dim;

            #include "helpers.cginc"
            #include "src/types.cginc" // includes fb.h

            // The GPU device's picture, for copying into RAM (docs/gpu.md).
            Texture2D<float4> _GpuTarget;
            #define GPU_STATE _SelfTexture2D
            #define GPU_WRITEBACK
            #include "src/gpu.cginc"

            // The bands this commit changes (commit_bands_changed in the harness's main.shader):
            // every core's writes.
            uint commit_bands_changed() {
                if (_Init) return 0xffffffff;
                uint changed = gpu_copy_bands();
                for (uint core = 0; core < CORES; core++) {
                    state_off = MC_BLOCK(core);
                    uint stalled = STATE_TEX(uint2(28, 0)).r;
                    changed |= STATE_TEX(uint2(41, 0)).g;   // the tick's writes
                    if (stalled == STALL_MEMOP_COPY || stalled == STALL_MEMOP_FILL) {
                        // a parallel copy or fill: the bands from its destination to its end
                        uint4 op = STATE_TEX(uint2(38, 0));
                        uint lo = (op.g >> 22) & 31, hi = min((op.g + op.b - 1) >> 22, 31u);
                        if (op.b != 0 && hi >= lo) changed |= ((2u << hi) - 1) & ~((1u << lo) - 1);
                    }
                }
                state_off = uint2(0, 0);
                return changed;
            }
            // Those, the ones of the commit before (the first commit after the start draws all)
            // and what the control pass changed since.
            uint blit_bands() {
                return commit_bands_changed() | STATE_TEX_HART(uint2(41, 0), 0).b | CONTROL_BANDS;
            }

            uint4 frag(blit_v2f i) : SV_Target {
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);
                uint2 pos = (uint2)i.vertex.xy;

                if (_Init && _InitRaw) {
                    float3 raw[6];
                    for (uint off = 0; off < 6; off++) {
                        raw[off] = _Data_RAM_RAW[pos + uint2((off * s_dim.x) % (s_dim.x*3), (off / 3) * s_dim.y)].rgb;
                    }
                    uint4 data = unpack_uint4(raw);
                    return data;
                }

                uint4 picture;
                if (!_Init && gpu_writeback(pos, picture)) return picture;
                uint4 result;
                if (pos.y < 64) {
                    // a core's own block: its state
                    uint core = pos.x / CORE_PITCH;
                    if (core >= CORES || pos.x % CORE_PITCH >= 64) return (uint4)0;
                    uint4 own = state_after_tick(pos);
                    mc_core = core;
                    state_off = MC_BLOCK(core);
                    pos = mc_texel_of(pos - state_off);
                    decode_for_commit();
                    result = commit(pos, own);
                    // A worker's one store that found its cache full is in its state until its next
                    // pass rewrites it: gone now, or the commits of a worker asleep would store it again.
                    if (core != 0 && pos.x == 8 && pos.y == 0) result.r = 0xffffffff;
                    if (core == 0 && pos.x == 41 && pos.y == 0) result.b = commit_bands_changed();   // for the control pass and the next commit
                } else {
                    // RAM: every core's writes, the highest core's last
                    result = RAM_TEX(pos);
                    for (uint core = 0; core < CORES; core++) {
                        mc_core = core;
                        state_off = MC_BLOCK(core);
                        if (core != 0 && STATE_TEX(uint2(41, 0)).r == 0) continue;   // a worker that stored nothing
                        decode_for_commit();
                        result = commit(pos, result);
                    }
                }
                return result;
            }
            ENDCG
        }

        Pass
        {
            Name "GPUControl"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry blit_bands_geom
            #pragma fragment frag
            #pragma editor_sync_compilation

            #define BLIT_BANDS
            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"

            // Its own bands, and the ones the commit just changed: the texture this pass draws
            // into is the one the commit read.
            uint blit_bands() {
                return _SelfTexture2D[uint2(41, 0)].b | CONTROL_BANDS;
            }

            uniform float4 _InputPointer;
            uniform uint _InputButtons, _InputKeySeq, _InputKeyCount;
            uniform uint _InputKeyCode0, _InputKeyCode1, _InputKeyCode2, _InputKeyCode3;
            uniform uint _HostMsLo, _HostMsHi;
            uniform uint _HostFlags;   // bit 0: the guest should start its desktop when it boots
            static uint _InputKey0, _InputKey1, _InputKey2, _InputKey3, _HostMs;
            // An answer to the guest's request (docs/fetch.md): its bytes, in the frame _FetchDeliver is set.
            uniform uint _FetchDeliver, _FetchSeq, _FetchLength, _FetchStatus;
            uniform uint _FetchInfo, _FetchW, _FetchH;   // a picture's size: one word, or its halves
            Texture2D<float4> _HostData;
            Texture2D<float4> _HostImage;
            // The sound card (docs/sound.md): its words move on in a pass that follows a mix.
            uniform uint _SoundCursorLo, _SoundCursorHi, _SoundMixed, _SoundRate;
            #define SOUND_CURSOR (_SoundCursorLo | (_SoundCursorHi << 16))
            #define SOUND_MIXED _SoundMixed
            #define SOUND_RATE _SoundRate
            #define GPU_SOUND
            // The GPU's camera draws all eight passes every frame.
            #define _GpuPasses 0xffu
            #define GPU_STATE _SelfTexture2D
            #define GPU_INPUT
            #include "src/gpu.cginc"

            // A key event is a Linux key code with bit 31 set while pressed.
            uint key_event(uint v) {
                return (v & 0xffff) | ((v >> 16) << 31);
            }

            uint4 frag(blit_v2f i) : SV_Target {
                _InputKey0 = key_event(_InputKeyCode0);
                _InputKey1 = key_event(_InputKeyCode1);
                _InputKey2 = key_event(_InputKeyCode2);
                _InputKey3 = key_event(_InputKeyCode3);
                _HostMs = _HostMsLo | (_HostMsHi << 16);
                return gpu_control((uint2)i.vertex.xy);
            }
            ENDCG
        }
    }
}
