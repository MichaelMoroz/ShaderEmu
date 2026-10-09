Shader "ShaderEmu/MachineTick"
{
    // The machine's CPUTick pass (experiments/rvc_opt/main.shader's), drawn by EmuMachine.cs with
    // VRCGraphics.Blit from the state texture into a 64x64 one that holds the CPU's state area.
    // It includes none of the GPU device's sources.
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

            #define CORES 1   // one core here (docs/multicore.md)
            #define STATE_TEX_HART(pos, hartidx) (_SelfTexture2D[uint2(pos) + uint2(hartidx % 2, hartidx / 2)])
            #define STATE_TEX(pos) (_SelfTexture2D[pos])
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

            uint4 frag(blit_v2f i) : SV_Target {
                _SelfTexture2D.GetDimensions(s_dim.x, s_dim.y);
                _Data_MTD_R.GetDimensions(m_dim.x, m_dim.y);

                uint2 pos = (uint2)i.vertex.xy;
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
                    while (i < ticks && !cpu.stall) {
                        // as many fast ticks in a row as possible, then one general tick
                        i += fast_run(ticks - i);
                        if (i < ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            i++;
                        }
                    }
                    xreg_store();
                }

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
    }
}
