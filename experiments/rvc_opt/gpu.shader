Shader "ShaderEmu/gpu"
{
    // The machine's GPU (docs/gpu.md). GPUDraw is a mesh drawn into the GPU's own colour and
    // depth target, its vertex shader reading the guest's command list from the state texture.
    // GPUControl is an update zone on the state texture: list drawn, input delivered.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _GpuTarget ("The GPU's own colour target", 2D) = "black" {}
        _HostFlags ("Host flags (1: the guest starts its desktop at boot)", Int) = 1
        _FetchDeliver ("An answer to the guest's request arrives this frame", Int) = 0
        _FetchSeq ("The request it answers", Int) = 0
        _FetchLength ("Its length in bytes", Int) = 0
        _FetchStatus ("Its status (200: fine)", Int) = 0
        _HostData ("Its bytes", 2D) = "black" {}
        _HostImage ("Or a picture, with _FetchDeliver 2", 2D) = "black" {}
        _NetData ("Packets for the guest, a row each", 2D) = "black" {}
        _FetchInfo ("A picture's width | height << 16", Int) = 0
        _FetchW ("or its width", Int) = 0
        _FetchH ("and its height", Int) = 0
        _Data_MTD_R ("ROM, first word of each texel", 2D) = "black" {}
        _Data_MTD_G ("ROM, second word", 2D) = "black" {}
        _Data_MTD_B ("ROM, third word", 2D) = "black" {}
        _Data_MTD_A ("ROM, fourth word", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "GPUDraw"
            ZTest LEqual
            ZWrite On

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            // The ROM, for textures that are files in it (FRAGMENT_RGB24).
            Texture2D<float4> _Data_MTD_R;
            Texture2D<float4> _Data_MTD_G;
            Texture2D<float4> _Data_MTD_B;
            Texture2D<float4> _Data_MTD_A;
            #define GPU_ROM
            // The pass this draw is (see FRAGMENT_PASS in gpu.h) and the passes drawn this frame,
            // one bit each. A host that only ever draws pass 0 leaves both at their defaults.
            uniform uint _GpuPass, _GpuPasses;
            #define GPU_STATE _State
            #define GPU_PASS_UNIFORMS
            #include "src/gpu.h"

            // In Unity the mesh carries its vertex number; here SV_VertexID is the same thing.
            gpu_varyings vert(uint id : SV_VertexID) {
                return gpu_vertex(id);
            }
            float4 frag(gpu_varyings i) : SV_Target {
                return gpu_fragment(i);
            }
            ENDCG
        }

        Pass
        {
            Name "GPUControl"
            ZTest Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex CustomRenderTextureVertexShader
            #pragma fragment frag

            #include "crt.cginc"
            #include "UnityCG.cginc"
            // The host's input for this frame: pointer position over the display panel and the
            // panel's size, both in window pixels; buttons; up to four key events (Linux key
            // code, bit 31 while pressed) numbered from _InputKeySeq.
            uniform float4 _InputPointer;
            uniform uint _InputButtons, _InputKeySeq, _InputKeyCount;
            uniform uint _HostMs;   // the host's clock, in milliseconds
            // bit 0: the guest should start its desktop when it boots; bits 8-15 and 16-23: the
            // largest screen this host shows, width and height in 16s of pixels (0: it does not say)
            uniform uint _HostFlags;
            uniform uint _GpuPasses;   // the passes the GPU draw made this frame, one bit each
            uniform uint _InputKey0, _InputKey1, _InputKey2, _InputKey3;
            // An answer to the guest's request (docs/fetch.md): its bytes, in the frame _FetchDeliver is set.
            uniform uint _FetchDeliver, _FetchSeq, _FetchLength, _FetchStatus;
            uniform uint _FetchInfo, _FetchW, _FetchH;   // a picture's size: one word, or its halves
            Texture2D<float4> _HostData;
            Texture2D<float4> _HostImage;
            // The sound card (docs/sound.md): the sample the host's ring starts at this frame,
            // whether it mixed (the device's words move only then), and its output rate.
            uniform uint _SoundCursor, _SoundMixed, _SoundRate;
            // The network (docs/lan.md): the guest's packets the host has taken, the packets it
            // delivered before this frame and how many arrive in it (rows of _NetData, 160 x 4,
            // four bytes a texel), and the machine's number (0: no link).
            uniform uint _NetTxAck, _NetRxSeq, _NetRxCount, _NetId;
            Texture2D<float4> _NetData;
            #define GPU_NET
            #define GPU_SOUND
            #define GPU_STATE _SelfTexture2D
            #define GPU_INPUT
            #include "src/gpu.h"

            uint4 frag(v2f_customrendertexture i) : SV_Target {
                uint2 dim;
                _SelfTexture2D.GetDimensions(dim.x, dim.y);
                return gpu_control(i.globalTexcoord.xy * dim);
            }
            ENDCG
        }
    }
}
