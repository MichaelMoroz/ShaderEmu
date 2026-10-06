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
        _HostFlags ("Host flags (1: the guest starts its desktop at boot)", Int) = 1
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
            #pragma fragment frag
            #pragma editor_sync_compilation

            #define PASS_COMMIT
            #define GPU_DEVICE

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

            // the CPU's state area as the tick left it; everything else as it was
            #define STATE_TEX_HART(pos, hartidx) (state_after_tick(uint2(pos) + uint2(hartidx % 2, hartidx / 2)))
            #define STATE_TEX(pos) (state_after_tick(pos))

            static uint2 s_dim;
            static uint2 m_dim;

            #include "helpers.cginc"
            #include "src/types.cginc" // includes fb.h

            // The GPU device's picture, for copying into RAM (docs/gpu.md).
            Texture2D<float4> _GpuTarget;
            #define GPU_STATE _SelfTexture2D
            #define GPU_WRITEBACK
            #include "src/gpu.cginc"

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
                decode_for_commit();
                return commit(pos);
            }
            ENDCG
        }

        Pass
        {
            Name "GPUControl"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            #pragma editor_sync_compilation

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"

            uniform float4 _InputPointer;
            uniform uint _InputButtons, _InputKeySeq, _InputKeyCount;
            uniform uint _InputKeyCode0, _InputKeyCode1, _InputKeyCode2, _InputKeyCode3;
            uniform uint _HostMsLo, _HostMsHi;
            uniform uint _HostFlags;   // bit 0: the guest should start its desktop when it boots
            static uint _InputKey0, _InputKey1, _InputKey2, _InputKey3, _HostMs;
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
