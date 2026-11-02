Shader "ShaderEmu/MachineTickLean"
{
    // The machine's CPUTick pass written again (shader_vm/tick.hlsl, copied verbatim to
    // lean/): the same state, targets and geometry as the machine has them.
    // HLSLPROGRAM: no Unity headers, which declare names the tick declares itself (_Time).
    Properties
    {
        _SelfTexture2D ("State before the pass", 2D) = "black" {}
        _TickState ("CPU state area after the tick", 2D) = "black" {}
        [ToggleUI] _Init ("Init", Int) = 0
        _Ticks ("Ticks per Frame", Int) = 2048

        _UartInLo ("UART input, characters 1-2", Int) = 0
        _UartInHi ("UART input, characters 3-4", Int) = 0
        _UdonUARTInTag ("UART input unique tag", Int) = 0

        _PlayerID ("Player ID", Int) = 0
        _Rtc0Lo ("RTC0 low half", Int) = 0
        _Rtc0Hi ("RTC0 high half", Int) = 0
        _Rtc1Lo ("RTC1 low half", Int) = 0
        _Rtc1Hi ("RTC1 high half", Int) = 0
        _MtimeLoLo ("The timer, low half", Int) = 0
        _MtimeLoHi ("The timer, high half", Int) = 0
        _LaneField ("The lanes' field drawn (docs/machine/lanes.md)", Int) = 1

        _Data_RAM ("The start image, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}


        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}

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

            HLSLPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry tick_geom
            #pragma fragment frag
            #pragma editor_sync_compilation
            #define RTC_HALVES
            uniform float4 _ScreenParams;
            #define TICK_GEOM_GUARD if ((uint)_ScreenParams.x != TICK_W || (uint)_ScreenParams.y != TICK_H) return;
            #define LANE_FIELD   // the lanes' field (docs/machine/lanes.md): the tick mesh's primitive after the cores' draws it
            #include "lean/tick.hlsl"
            ENDHLSL
        }
    }
}
