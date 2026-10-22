Shader "ShaderEmu/gvm_devices"
{
    // gpu_vm's devices (gpu_vm/platform.md): the RISC-V machine's GPU, control words and sound card
    // (experiments/rvc_opt/gpu.shader, sound.shader and their src/gpu.h, src/sound.h) over gpu_vm's RAM texture
    // (8192 texels wide from address 0, the device window moved down by 0x86000000) and its ROM texture: GVM_RAM.
    // Drawn by rvc_harness --gvm (harness/apps/gvm_backend11.cpp), never by Unity.
    Properties
    {
        _State ("gpu_vm's RAM texture", 2D) = "black" {}
        _GvmRom ("gpu_vm's ROM texture", 2D) = "black" {}
    }
    SubShader
    {
        Pass
        {
            Name "GPUDraw"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<uint4> _State;
            Texture2D<uint4> _GvmRom;
            uniform uint _GpuPass, _GpuPasses;
            #define GVM_RAM
            #define GPU_ROM
            #define GPU_STATE _State
            #define GPU_PASS_UNIFORMS
            #include "../../experiments/rvc_opt/src/gpu.h"
            gpu_varyings vert(uint id : SV_VertexID) { return gpu_vertex(id); }
            float4 frag(gpu_varyings i) : SV_Target { return gpu_fragment(i); }
            ENDCG
        }

        // The control words as zones: the host's viewport is the zone in a scratch texture, which it then copies into
        // RAM at (_GvmZoneX, _GvmZoneY); every pixel reads RAM, none the texture it draws.
        Pass
        {
            Name "GPUControl"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<uint4> _State;
            Texture2D<uint4> _GvmS0;   // the tick's state textures 0 and 3 (texels 16 and 19 of each core's row)
            Texture2D<uint4> _GvmS3;
            uniform uint _GvmZoneX, _GvmZoneY, _GvmPasses, _GvmWorkers, _GvmParkLo, _GvmParkHi;
            uniform float4 _InputPointer;
            uniform uint _InputButtons, _InputKeySeq, _InputKeyCount;
            uniform uint _HostMs;
            uniform uint _UdonUARTInChar, _UdonUARTInTag;
            uniform uint _HostState0, _HostState1, _HostState2, _HostState3, _HostState4, _HostState5, _HostState6, _HostState7;
            #define HOST_STATE_WORDS(n) ((n) == 0 ? _HostState0 : (n) == 1 ? _HostState1 : (n) == 2 ? _HostState2 : (n) == 3 ? _HostState3 : (n) == 4 ? _HostState4 : (n) == 5 ? _HostState5 : (n) == 6 ? _HostState6 : (n) == 7 ? _HostState7 : 0u)
            uniform uint _HostFlags;
            uniform uint _GpuPasses;
            uniform uint _CountMerged;
            #define GPU_COUNT_MERGE
            uniform uint _InputKey0, _InputKey1, _InputKey2, _InputKey3;
            uniform uint _FetchDeliver, _FetchSeq, _FetchLength, _FetchStatus;
            uniform uint _FetchInfo, _FetchW, _FetchH;
            uniform uint _FetchOffset;
            Texture2D<float4> _HostData;
            Texture2D<float4> _HostImage;
            uniform uint _SoundCursor, _SoundMixed, _SoundRate;
            uniform uint _NetTxAck, _NetRxSeq, _NetRxCount, _NetId;
            Texture2D<float4> _NetData;
            #define GVM_RAM
            #define CORES 256
            #define GPU_NET
            #define GPU_SOUND
            #define GPU_STATE _State
            #define GPU_INPUT
            #include "../../experiments/rvc_opt/src/gpu.h"
            float4 vert(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0); }
            uint4 frag(float4 at : SV_Position) : SV_Target { return gpu_control(uint2(at.xy) + uint2(_GvmZoneX, _GvmZoneY)); }
            ENDCG
        }

        // The picture copied into RAM (SUBMIT_WRITEBACK, SUBMIT_INTO: src/gpu.h's gpu_writeback, which the RISC-V
        // machine's commit makes): four draws of the destination's rows into RAM, word _GvmComp of each texel a draw
        // (the host's write mask), a pixel with no word of the picture discarded. The copy is the one the control
        // pass noted in the last frame, as the control words were before this frame's devices (_GvmCtl: RAM from
        // 0x01000000, 256 texels).
        Pass
        {
            Name "GPUWriteback"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<uint4> _GvmCtl;
            Texture2D<float4> _GpuTarget;
            uniform uint _GvmComp, _GvmRows;
            #define CTL(i) _GvmCtl.Load(int3((i) - 0x700000u, 0, 0))
            #define GVM_RAM
            #define GPU_STATE _GvmCtl   // (gpu.h's RAM functions, which this pass does not call)
            #include "../../experiments/rvc_opt/src/gpu.h"
            // the copy: its first word (of RAM, RISC-V's numbering: (address - 0x80000000) / 4), width, height, row length
            bool copy_of(out uint first, out uint width, out uint height, out uint row) {
                uint4 copy = CTL(GPU_COPY);
                first = width = height = row = 0;
                if (copy.r == 0) return false;
                if (copy.r & SUBMIT_INTO) {
                    // (the address is gpu_vm's: below the ROM, 0x86000000 less than the RISC-V machine's)
                    first = ((copy.g + 0x86000000u) & 0x7fffffff) >> 2;
                    width = copy.b & 0xffff;
                    height = copy.b >> 16;
                    row = max(copy.a, width);
                } else {
                    uint4 disp = CTL(GPU_DISPLAY);
                    first = GPU_PIXELS * 4;
                    width = row = disp.g;
                    height = disp.b;
                }
                return width != 0 && height != 0;
            }
            float4 vert(uint id : SV_VertexID) : SV_Position {
                uint first, width, height, row;
                if (!copy_of(first, width, height, row)) return float4(-2, -2, 0, 1);
                // gpu_vm's texels: the RISC-V index less 0x600000
                uint t0 = first / 4 - 0x600000u, t1 = (first + row * (height - 1) + width - 1) / 4 - 0x600000u;
                float y0 = (float)(t0 / 8192u), y1 = (float)(t1 / 8192u + 1);
                float2 c = float2(id == 1 || id == 2 || id == 4 ? 1.0 : 0.0, id == 2 || id == 4 || id == 5 ? 1.0 : 0.0);
                float y = lerp(y0, y1, c.y) / (float)_GvmRows;
                return float4(c.x * 2.0 - 1.0, 1.0 - y * 2.0, 0, 1);
            }
            uint4 frag(float4 at : SV_Position) : SV_Target {
                uint index = (uint)at.y * 8192u + (uint)at.x + 0x600000u, first, width, height, row;
                if (index - 0x600000u >= 0x1e0000u || !copy_of(first, width, height, row)) discard;   // (what a GPU program may own)
                uint word = index * 4 + _GvmComp;
                if (word < first) discard;
                uint p = word - first, y = p / row, x = p - y * row;
                if (x >= width || y >= height) discard;
                uint3 c = (uint3)(_GpuTarget.Load(int3(x, y, 0)).rgb * 255.0 + 0.5);
                return (uint4)((c.r << 16) | (c.g << 8) | c.b);
            }
            ENDCG
        }

        Pass
        {
            Name "SoundMix"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<uint4> _State;
            Texture2D<uint4> _GvmRom;
            uniform uint _SoundCursor;
            #define SOUND_CURSOR _SoundCursor
            #define SOUND_MIXED 1
            #define SOUND_RATE 0
            #define GVM_RAM
            #define GPU_ROM
            #define GPU_STATE _State
            #define GPU_SOUND
            #include "../../experiments/rvc_opt/src/gpu.h"
            float4 vert(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0); }
            float4 frag(float4 at : SV_Position) : SV_Target { return float4(sound_mix((uint2)at.xy), 0.0, 1.0); }
            ENDCG
        }
    }
}
