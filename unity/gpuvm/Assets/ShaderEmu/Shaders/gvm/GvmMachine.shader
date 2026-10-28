Shader "ShaderEmu/GvmMachine"
{
    // gpu_vm's machine around the tick (gpu_vm/platform.md, harness/apps/gvm_backend11.cpp), drawn by EmuMachine.cs
    // with VRCGraphics.Blit; none reads the texture it draws. RAM is 8192 texels wide from address 0; the device
    // passes are the RISC-V machine's (src/gpu.cginc) with GVM_RAM. Numbers past 24 bits come in 16-bit halves.
    Properties
    {
        _Ram ("RAM", 2D) = "black" {}
        _GvmRamR ("Boot RAM, word 0 of a texel", 2D) = "black" {}
        _GvmRamG ("Boot RAM, word 1", 2D) = "black" {}
        _GvmRamB ("Boot RAM, word 2", 2D) = "black" {}
        _GvmRamA ("Boot RAM, word 3", 2D) = "black" {}
        _GvmRomR ("ROM, word 0 of a texel", 2D) = "black" {}
        _GvmRomG ("ROM, word 1", 2D) = "black" {}
        _GvmRomB ("ROM, word 2", 2D) = "black" {}
        _GvmRomA ("ROM, word 3", 2D) = "black" {}
        _GvmSnapR ("A snapshot's cores, word 0 of a texel", 2D) = "black" {}
        _GvmSnapG ("A snapshot's cores, word 1", 2D) = "black" {}
        _GvmSnapB ("A snapshot's cores, word 2", 2D) = "black" {}
        _GvmSnapA ("A snapshot's cores, word 3", 2D) = "black" {}
        _Zone ("The control zone's scratch", 2D) = "black" {}
        _HostData ("A fetch's bytes", 2D) = "black" {}
        _HostImage ("Or a picture", 2D) = "black" {}
        _NetData ("Packets for the guest", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend One Zero

        CGINCLUDE
        #include "UnityCG.cginc"
        #include "../MachineBlit.cginc"
        Texture2D<uint4> _Ram;
        uniform uint _GvmRows;
        // a word of a boot image's four textures (ShaderEmuGvm.cs: 2048 texels a row, texel 0 in the last row)
        uint lane_word(Texture2D<float4> lane, uint t, uint2 dim) {
            uint4 b = (uint4)(lane.Load(int3(t % dim.x, dim.y - 1 - t / dim.x, 0)) * 255.0 + 0.5);
            return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
        }
        // a rectangle of the target as Blit's geometry (row 0 is clip y = 1)
        void rect(inout TriangleStream<blit_v2f> s, float x0, float y0, float w, float h, float W, float H) {
            float l = x0 / W * 2 - 1, r = (x0 + w) / W * 2 - 1, t = 1 - y0 / H * 2, b = 1 - (y0 + h) / H * 2;
            blit_v2f o;
            o.vertex = float4(l, t, 0.5, 1); s.Append(o);
            o.vertex = float4(r, t, 0.5, 1); s.Append(o);
            o.vertex = float4(l, b, 0.5, 1); s.Append(o);
            o.vertex = float4(r, b, 0.5, 1); s.Append(o);
            s.RestartStrip();
        }
        ENDCG

        Pass
        {
            // 0: RAM at power-on, from the boot image (zeros past it)
            Name "InitRam"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            Texture2D<float4> _GvmRamR, _GvmRamG, _GvmRamB, _GvmRamA;
            uint4 frag(blit_v2f i) : SV_Target {
                uint2 p = (uint2)i.vertex.xy, d;
                _GvmRamR.GetDimensions(d.x, d.y);
                uint t = p.y * 8192 + p.x;
                if (t >= d.x * d.y) return 0;
                return uint4(lane_word(_GvmRamR, t, d), lane_word(_GvmRamG, t, d), lane_word(_GvmRamB, t, d), lane_word(_GvmRamA, t, d));
            }
            ENDCG
        }

        Pass
        {
            // 1: the ROM texture from its four textures
            Name "InitRom"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            Texture2D<float4> _GvmRomR, _GvmRomG, _GvmRomB, _GvmRomA;
            uint4 frag(blit_v2f i) : SV_Target {
                uint2 p = (uint2)i.vertex.xy, d;
                _GvmRomR.GetDimensions(d.x, d.y);
                uint t = p.y * 8192 + p.x;
                if (t >= d.x * d.y) return 0;
                return uint4(lane_word(_GvmRomR, t, d), lane_word(_GvmRomG, t, d), lane_word(_GvmRomB, t, d), lane_word(_GvmRomA, t, d));
            }
            ENDCG
        }

        Pass
        {
            // 2: state texture _GvmTarget of a set at power-on: core 0 at the kernel's entry, supervisor, r5 the device
            // tree; every other texel 0 (gvm_backend11.cpp's init)
            Name "InitState"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            uniform uint _GvmTarget, _GvmEntryLo, _GvmEntryHi, _GvmBH;   // (GEO_BH of tick.cginc)
            uint4 frag(blit_v2f i) : SV_Target {
                uint2 p = (uint2)i.vertex.xy;
                if (p.x >= 8 || p.y >= _GvmBH) return 0;
                uint T = (p.x * _GvmBH + p.y) * 8 + _GvmTarget;
                if (T == 1) return uint4(0, 0x1000, 0, 0);
                if (T == 16) return uint4((_GvmEntryLo & 0xffff) | (_GvmEntryHi << 16), 8, 0, 0);
                if (T == 18) return uint4(0, 0, 0, 0x9e3779b9u);
                return 0;
            }
            ENDCG
        }

        Pass
        {
            // 3: the workers the kernel laid out (gvm_backend11.cpp's setWorkers), into texture _GvmTarget of the set
            // the next tick reads: cores _GvmFrom + 1 to _GvmTo start at the firmware, cores _GvmTo + 1 to _GvmFrom halt
            Name "Workers"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            uniform uint _GvmTarget, _GvmFrom, _GvmTo, _GvmBH, _GvmNTex;   // (GEO_BH, NTEX of tick.cginc)
            uint4 frag(blit_v2f i) : SV_Target {
                uint2 p = (uint2)i.vertex.xy;
                uint c = (p.y / 16) * 128 + p.x / 8, ry = p.y % 16, T = ((p.x % 8) * _GvmBH + ry) * 8 + _GvmTarget;
                if (ry >= _GvmBH || T >= _GvmNTex) discard;
                if (c > _GvmFrom && c <= _GvmTo) {
                    uint4 s = T >= 40 ? uint4(0xffffffffu, 0, 0xffffffffu, 0) : 0;
                    if (T == 1) s.y = c;
                    if (T == 16) s = uint4(0x400, 8, 0, 0);   // GVM_FW_ENTRY, supervisor
                    if (T == 18) s.w = 0x9e3779b9u ^ (c * 0x85ebca6bu);
                    return s;
                }
                if (c > _GvmTo && c <= _GvmFrom && T == 18) return uint4(0, 0, 4, 0);   // halted
                discard;
                return 0;
            }
            ENDCG
        }

        Pass
        {
            // 4: a zone of the control pass (devices.shader's GPUControl) into the scratch's rectangle at its top left,
            // reading RAM; from pixel _GvmZoneSplit on the row is a second zone
            Name "GPUControl"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry geom
            #pragma fragment frag
            Texture2D<uint4> _GvmS0, _GvmS3;
            uniform uint _GvmZoneX, _GvmZoneY, _GvmZoneW, _GvmZoneH, _GvmZoneSplit, _GvmZone2X, _GvmZone2Y;
            uniform uint _GvmPassesLo, _GvmPassesHi, _GvmWorkers, _GvmParkLo, _GvmParkHi, _GvmGeo, _GvmListDrawn;
            static uint _GvmPasses;
            uniform float4 _InputPointer;
            uniform uint _InputButtons, _InputKeySeq, _InputKeyCount;
            uniform uint _InputKeyCode0, _InputKeyCode1, _InputKeyCode2, _InputKeyCode3;
            static uint _InputKey0, _InputKey1, _InputKey2, _InputKey3, _HostMs;
            uniform uint _HostMsLo, _HostMsHi, _HostFlags;
            uniform uint _HostS0, _HostS1, _HostS2, _HostS3, _HostS4, _HostS5, _HostS6, _HostS7;
            uniform uint _HostS8, _HostS9, _HostS10, _HostS11, _HostS12, _HostS13, _HostS14, _HostS15;
            uint host_state_word(uint n) {
                uint lo = n == 0 ? _HostS0 : n == 1 ? _HostS2 : n == 2 ? _HostS4 : n == 3 ? _HostS6 : n == 4 ? _HostS8 : n == 5 ? _HostS10 : n == 6 ? _HostS12 : _HostS14;
                uint hi = n == 0 ? _HostS1 : n == 1 ? _HostS3 : n == 2 ? _HostS5 : n == 3 ? _HostS7 : n == 4 ? _HostS9 : n == 5 ? _HostS11 : n == 6 ? _HostS13 : _HostS15;
                return (lo & 0xffff) | (hi << 16);
            }
            #define HOST_STATE_WORDS(n) host_state_word(n)
            uniform uint _NetId, _NetTxAckLo, _NetTxAckHi, _NetRxSeqLo, _NetRxSeqHi, _NetRxCount;
            static uint _NetTxAck, _NetRxSeq;
            Texture2D<float4> _NetData;
            #define GPU_NET
            uniform uint _FetchDeliver, _FetchSeq, _FetchLength, _FetchStatus;
            uniform uint _FetchInfo, _FetchW, _FetchH, _FetchOffset;
            Texture2D<float4> _HostData;
            Texture2D<float4> _HostImage;
            uniform uint _SoundCursorLo, _SoundCursorHi, _SoundMixed, _SoundRate;
            #define SOUND_CURSOR (_SoundCursorLo | (_SoundCursorHi << 16))
            #define SOUND_MIXED _SoundMixed
            #define SOUND_RATE _SoundRate
            #define GPU_SOUND
            #define _GpuPasses 0xffu
            #define _CountMerged 1u
            #define GPU_COUNT_MERGE
            uniform uint _UartInLo, _UartInHi, _UdonUARTInTag;
            #define CON_IN_TAG _UdonUARTInTag
            #define CON_IN_CHARS (_UartInLo | (_UartInHi << 16))
            #define GVM_RAM
            #define CORES 256
            #define GPU_STATE _Ram
            #define GPU_INPUT
            #include "../src/gpu.cginc"

            [maxvertexcount(4)]
            void geom(triangle blit_v2f corners[3], uint which : SV_PrimitiveID, inout TriangleStream<blit_v2f> s) {
                if (which == 0) rect(s, 0, 0, _GvmZoneW, _GvmZoneH, 8192, 3);
            }
            uint key_event(uint v) { return (v & 0xffff) | ((v >> 16) << 31); }
            uint4 frag(blit_v2f i) : SV_Target {
                _InputKey0 = key_event(_InputKeyCode0);
                _InputKey1 = key_event(_InputKeyCode1);
                _InputKey2 = key_event(_InputKeyCode2);
                _InputKey3 = key_event(_InputKeyCode3);
                _HostMs = (_HostMsLo & 0xffff) | (_HostMsHi << 16);
                _NetTxAck = (_NetTxAckLo & 0xffff) | (_NetTxAckHi << 16);
                _NetRxSeq = (_NetRxSeqLo & 0xffff) | (_NetRxSeqHi << 16);
                _GvmPasses = (_GvmPassesLo & 0xffff) | (_GvmPassesHi << 16);
                uint2 p = (uint2)i.vertex.xy;
                return gpu_control(p.x >= _GvmZoneSplit ? uint2(p.x - _GvmZoneSplit + _GvmZone2X, _GvmZone2Y) : p + uint2(_GvmZoneX, _GvmZoneY));
            }
            ENDCG
        }

        Pass
        {
            // 5: the scratch's zone (and its second, after _GvmZoneSplit) into RAM where it stands for
            Name "ZoneBack"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma geometry geom
            #pragma fragment frag
            Texture2D<uint4> _Zone;
            uniform uint _GvmZoneX, _GvmZoneY, _GvmZoneW, _GvmZoneH, _GvmZoneSplit, _GvmZone2X, _GvmZone2Y;
            [maxvertexcount(8)]
            void geom(triangle blit_v2f corners[3], uint which : SV_PrimitiveID, inout TriangleStream<blit_v2f> s) {
                if (which != 0) return;
                uint wa = min(_GvmZoneW, _GvmZoneSplit);
                rect(s, _GvmZoneX, _GvmZoneY, wa, _GvmZoneH, 8192, _GvmRows);
                if (_GvmZoneSplit < _GvmZoneW) rect(s, _GvmZone2X, _GvmZone2Y, _GvmZoneW - _GvmZoneSplit, 1, 8192, _GvmRows);
            }
            uint4 frag(blit_v2f i) : SV_Target {
                uint2 p = (uint2)i.vertex.xy;
                uint wa = min(_GvmZoneW, _GvmZoneSplit);
                if (p.x >= _GvmZoneX && p.x < _GvmZoneX + wa && p.y >= _GvmZoneY && p.y < _GvmZoneY + _GvmZoneH)
                    return _Zone[p - uint2(_GvmZoneX, _GvmZoneY)];
                return _Zone[uint2(p.x - _GvmZone2X + _GvmZoneSplit, 0)];
            }
            ENDCG
        }

        Pass
        {
            // 6: the control words (RAM from 0x01000000, 256 texels) for the next frame's writeback
            Name "CtlCopy"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            uint4 frag(blit_v2f i) : SV_Target { return _Ram[uint2((uint)i.vertex.x, 128)]; }
            ENDCG
        }

        Pass
        {
            // 7: the script's readback, a pixel a word, a row a round, laid out as the RISC-V machine's
            // (Readback.shader): its state row 0 made from core 0's texels (the registers, ops as instructions, passes
            // as commits, the pc, why its pass ended), 64 control texels, the network row's window, counts and console
            Name "Readback"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<uint4> _GvmR0, _GvmR1, _GvmR2, _GvmR3, _GvmR4, _GvmR5, _GvmR6, _GvmR7;
            Texture2D<uint4> _GvmPending;   // GvmOpsBlit's pass 3: cores with operations still queued (texel 12)
            uniform uint _Row, _GvmPassesLo, _GvmPassesHi;
            static const uint Words = 1032;
            float4 vert(appdata_img v) : SV_Position {
                return float4(v.texcoord.x * 2.0 - 1.0, 1.0 - 2.0 * (_Row + v.texcoord.y) / 32.0, 0.5, 1.0);
            }
            uint pick4(uint4 t, uint k) { return k == 0 ? t.r : k == 1 ? t.g : k == 2 ? t.b : t.a; }
            uniform uint _GvmOne;   // (GvmTickOne's one target: core 0's texel T at column T >> 3, row T & 7, in _GvmR0)
            uint core0(uint T, uint k) {   // core 0's state texel T (0-23): target T & 7, pixel T >> 3 at (0, T >> 3)
                [branch] if (_GvmOne != 0u) return pick4(_GvmR0.Load(int3(T >> 3, T & 7, 0)), k);
                int3 at = int3(0, T >> 3, 0);
                uint t = T & 7;
                uint4 v = t == 0 ? _GvmR0.Load(at) : t == 1 ? _GvmR1.Load(at) : t == 2 ? _GvmR2.Load(at) : t == 3 ? _GvmR3.Load(at)
                        : t == 4 ? _GvmR4.Load(at) : t == 5 ? _GvmR5.Load(at) : t == 6 ? _GvmR6.Load(at) : _GvmR7.Load(at);
                return pick4(v, k);
            }
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint n = min((uint)at.x, Words - 1), texel = n / 4, k = n & 3, v = 0;
                if (texel < 64) {
                    int reg = (int)(texel - 28) * 4 + (int)k - 3;   // x1-x31 where the RISC-V row has them
                    if (texel == 28 && k == 1) v = core0(19, 1);
                    else if (texel == 28 && k == 2) v = (_GvmPassesLo & 0xffff) | (_GvmPassesHi << 16);
                    else if (texel == 36 && k == 3) v = core0(16, 0);
                    else if (texel == 40 && k == 2) v = (core0(19, 3) & 0xff) | (core0(19, 2) << 8);
                    else if (texel == 11 && k == 3) v = 0xffffffffu;
                    else if (texel == 12 && k == 0) v = _GvmPending.Load(int3(0, 0, 0)).x;
                    else if (texel >= 28 && reg >= 1 && reg <= 31) v = core0((uint)reg >> 2, (uint)reg & 3);
                } else if (texel < 128) v = pick4(_Ram[uint2(texel - 64, 128)], k);
                else {
                    uint more = texel - 128;
                    more = more < 40 ? more : more < 112 ? 0x200 + (more - 40) : 40 + (more - 112);
                    v = pick4(_Ram[uint2(6144 + more, 181)], k);
                }
                return float4(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24) / 255.0;
            }
            ENDCG
        }

        Pass
        {
            // 8: zeros (the operations' progress at power-on)
            Name "Zero"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            uint4 frag(blit_v2f i) : SV_Target { return 0; }
            ENDCG
        }

        Pass
        {
            // 9: state texture _GvmTarget of a set from a world snapshot (gvm_cpu --save-world; ShaderEmuGvm.cs): core c's
            // texel T (40 a core) is texel 40 c + T of the four lanes; core c's pixel j at (slot * 8 + j / BH, band * 16 +
            // j % BH), its band c / 128, slot c % 128, texel T = 8 j + _GvmTarget (InitState's order). The write cache's
            // texels (40 on) are empty, as after a commit
            Name "SnapState"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment frag
            Texture2D<float4> _GvmSnapR, _GvmSnapG, _GvmSnapB, _GvmSnapA;
            uniform uint _GvmTarget, _GvmBH, _GvmSnapCores;
            uint4 frag(blit_v2f i) : SV_Target {
                uint2 p = (uint2)i.vertex.xy, d;
                uint jy = p.y % 16, c = (p.y / 16) * 128 + p.x / 8;
                if (jy >= _GvmBH || c >= _GvmSnapCores) return 0;
                uint T = ((p.x % 8) * _GvmBH + jy) * 8 + _GvmTarget;
                if (T >= 40) return 0;
                _GvmSnapR.GetDimensions(d.x, d.y);
                uint t = c * 40 + T;
                return uint4(lane_word(_GvmSnapR, t, d), lane_word(_GvmSnapG, t, d), lane_word(_GvmSnapB, t, d), lane_word(_GvmSnapA, t, d));
            }
            ENDCG
        }
    }
}
