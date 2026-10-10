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

            #include "MachineCommit.cginc"

            // Those, the ones of the commit before (the first commit after the start draws all)
            // and what the control pass changed since.
            uint blit_bands() {
                return commit_bands_changed() | STATE_TEX_HART(uint2(41, 0), 0).b | CONTROL_BANDS;
            }

            uint4 frag(blit_v2f i) : SV_Target {
                return commit_texel((uint2)i.vertex.xy);
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
            // Where the machine is shown (docs/holodeck.md): eight words, each in two halves.
            uniform uint _HostS0, _HostS1, _HostS2, _HostS3, _HostS4, _HostS5, _HostS6, _HostS7;
            uniform uint _HostS8, _HostS9, _HostS10, _HostS11, _HostS12, _HostS13, _HostS14, _HostS15;
            uint host_state_word(uint n) {
                uint lo = n == 0 ? _HostS0 : n == 1 ? _HostS2 : n == 2 ? _HostS4 : n == 3 ? _HostS6 : n == 4 ? _HostS8 : n == 5 ? _HostS10 : n == 6 ? _HostS12 : _HostS14;
                uint hi = n == 0 ? _HostS1 : n == 1 ? _HostS3 : n == 2 ? _HostS5 : n == 3 ? _HostS7 : n == 4 ? _HostS9 : n == 5 ? _HostS11 : n == 6 ? _HostS13 : _HostS15;
                return (lo & 0xffff) | (hi << 16);
            }
            #define HOST_STATE_WORDS(n) host_state_word(n)
            // The network (docs/lan.md): what the guest's packets were taken up to, what this pass
            // delivers (rows of _NetData), and the machine's number. Counts come in two halves.
            uniform uint _NetId, _NetTxAckLo, _NetTxAckHi, _NetRxSeqLo, _NetRxSeqHi, _NetRxCount;
            static uint _NetTxAck, _NetRxSeq;
            Texture2D<float4> _NetData;
            #define GPU_NET
            // An answer to the guest's request (docs/fetch.md): its bytes, in the frame _FetchDeliver is set.
            uniform uint _FetchDeliver, _FetchSeq, _FetchLength, _FetchStatus;
            uniform uint _FetchInfo, _FetchW, _FetchH;   // a picture's size: one word, or its halves
            uniform uint _FetchOffset;   // _FetchDeliver 3: where in the file a picture carries this part begins
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
                _NetTxAck = (_NetTxAckLo & 0xffff) | (_NetTxAckHi << 16);
                _NetRxSeq = (_NetRxSeqLo & 0xffff) | (_NetRxSeqHi << 16);
                return gpu_control((uint2)i.vertex.xy);
            }
            ENDCG
        }

        Pass
        {
            // The tick's eight targets, into the tick's texture as the state has its rows (as
            // main.shader's TICK_UNPACK): core 0's rectangle and the workers' band. What the tick
            // did not draw (a worker with nothing to run) is the state as it was.
            Name "Unpack"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag

            #define PASS_TICK
            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"

            float _Init;
            Texture2D<uint4> _TickOut0, _TickOut1, _TickOut2, _TickOut3, _TickOut4, _TickOut5, _TickOut6, _TickOut7;

            // the state before the tick, as MachineTick.shader reads it
            static uint hart = 0;
            static uint2 state_off = uint2(0, 0);
            static uint mc_core = 0;
            static uint mc_bits_now = 6;
            static uint mc_size_now = 4096;
            static uint mc_tile0 = 0;
            static uint4 mc_geo, mc_geo1, mc_geo2;
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
            #include "src/types.cginc"
            #include "src/mc.cginc"

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

            uint4 frag(blit_v2f i) : SV_Target {
                uint2 pos = (uint2)i.vertex.xy;
                if (pos.x < 64) return tick_out(pos.x & 7, uint2(pos.x >> 3, pos.y));
                if (pos.y >= WORKER_BAND_ROWS) return (uint4)0;
                if (_Init) return _SelfTexture2D[pos];
                MC_GEO_READ
                if ((mc_geo.r & 1) != 0) return (uint4)0;   // being laid out anew
                uint tile = (pos.x - MC_STRIP_X) >> 3;
                hart = mc_core_at(tile);
                if (hart == 0 || mc_idle()) return _SelfTexture2D[pos];
                // a worker's texel: in its block's column for this tile
                return tick_out(pos.x & 7, uint2((MC_STRIP_X >> 3) + TICK_GAP + mc_col0 + (tile - mc_tile0), pos.y));
            }
            ENDCG
        }
    }
}
