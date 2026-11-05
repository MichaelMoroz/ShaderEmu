Shader "ShaderEmu/gpu"
{
    // The machine's GPU (docs/machine/gpu.md). GPUDraw is a mesh drawn into the GPU's own colour and
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
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
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
            // The pass this draw is (see FRAGMENT_PASS in gpu.h) and the passes drawn this frame,
            // one bit each. A host that only ever draws pass 0 leaves both at their defaults.
            uniform uint _GpuPass, _GpuPasses;
            #define GPU_STATE _State
            #include "src/gpu_draw.h"

            // In Unity the mesh carries its vertex number; here SV_VertexID is the same thing.
            gpu_varyings vert(uint id : SV_VertexID) {
                return gpu_vertex(id, _GpuPass);
            }
            float4 frag(gpu_varyings i) : SV_Target {
                return gpu_fragment(i);
            }
            ENDCG
        }

        Pass
        {
            // The list's uploads (CMD_UPLOAD) into the VRAM, before its passes; the host then makes the VRAM's mip levels.
            Name "GPUUpload"
            ZTest Off
            ZWrite Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            Texture2D<float4> _Data_ROM;
            #define GPU_ROM
            #define GPU_STATE _State
            #define _GpuPass 16u   // (the uploads' bit of the draw IDs)
            #include "src/gpu_draw.h"

            gpu_varyings vert(uint id : SV_VertexID) {
                return gpu_vertex(id, _GpuPass);
            }
            float4 frag(gpu_varyings i) : SV_Target {
                return gpu_upload_fragment(i);
            }
            ENDCG
        }

        Pass
        {
            // The list's copies (CMD_COPY) of rectangles of its picture into slots of the VRAM, after all its passes
            // (docs/machine/gpu.md, "Copies"): the target read, the VRAM's level 0 written.
            Name "GPUCopy"
            ZTest Off
            ZWrite Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            #define GPU_STATE _State
            #define _GpuPass 17u   // (the copies' bit of the draw IDs)
            #include "src/gpu_draw.h"

            gpu_varyings vert(uint id : SV_VertexID) {
                return gpu_vertex(id, _GpuPass);
            }
            float4 frag(gpu_varyings i) : SV_Target {
                return gpu_copy_fragment(i);
            }
            ENDCG
        }

        Pass
        {
            // The vertex front end (docs/machine/gpu.md, "The vertex front end"): every vertex a command claims made once,
            // from the list in RAM, into six targets of GPU_DRAW_IDS_W x GPU_DRAW_IDS_H, after the draw IDs and before the
            // uploads; the passes after it read their vertices from there.
            Name "GPUVertices"
            ZTest Off
            ZWrite Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            Texture2D<float4> _Data_ROM;   // (packed vertices may be a file's bytes where it lies in the ROM)
            #define GPU_ROM
            #define GPU_STATE _State
            #define _GpuPass 0u
            #include "src/gpu_draw.h"

            float4 vert(uint id : SV_VertexID) : SV_Position {
                return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
            }
            gpu_made frag(float4 p : SV_Position) {
                uint2 at = uint2(p.xy);
                return gpu_front_fragment(at.y * GPU_DRAW_IDS_W + at.x);
            }
            ENDCG
        }

        Pass
        {
            // Each mesh vertex's command and passes (gpu_draw_ids_fragment), a texel each of a GPU_DRAW_IDS_W x GPU_DRAW_IDS_H
            // R32G32_UINT target, drawn first every frame a list is drawn: the upload pass and the GPUDraw passes read it.
            Name "GPUDrawIds"
            ZTest Off
            ZWrite Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            uniform uint _GpuPasses;
            #define GPU_STATE _State
            #include "src/gpu_draw.h"

            // one triangle over the target
            float4 vert(uint id : SV_VertexID) : SV_Position {
                return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
            }
            int2 frag(float4 p : SV_Position) : SV_Target {
                uint2 at = uint2(p.xy);
                return asint(gpu_draw_ids_fragment(at.y * GPU_DRAW_IDS_W + at.x, _GpuPasses));
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
            uniform uint _UdonUARTInChar, _UdonUARTInTag;   // the console's input (docs/machine/console.md)
            // bit 0: the guest should start its desktop when it boots; bits 8-15 and 16-23: the
            // largest screen this host shows, width and height in 16s of pixels (0: it does not say)
            // What the host says of where the machine is shown (docs/world/holodeck.md): word 0, the head's three, and where the right hand points.
            uniform uint _HostState0, _HostState1, _HostState2, _HostState3, _HostState4, _HostState5, _HostState6, _HostState7;
            #define HOST_STATE_WORDS(n) ((n) == 0 ? _HostState0 : (n) == 1 ? _HostState1 : (n) == 2 ? _HostState2 : (n) == 3 ? _HostState3 : (n) == 4 ? _HostState4 : (n) == 5 ? _HostState5 : (n) == 6 ? _HostState6 : (n) == 7 ? _HostState7 : 0u)
            uniform uint _HostFlags;
            uniform uint _GpuPasses;   // the passes the GPU draw made this frame, one bit each
            // 1: the harness did not draw the list this frame (RVC_LIST_LAZY), which then stays submitted
            uniform uint _GpuListSkipped;
            #define GPU_LIST_SKIP
            uniform uint _CountMerged;   // the counters (src/gpu.h): 0 a host that never merges, 2 merged this frame
            #define GPU_COUNT_MERGE
            uniform uint _InputKey0, _InputKey1, _InputKey2, _InputKey3;
            // An answer to the guest's request (docs/system/fetch.md): its bytes, in the frame _FetchDeliver is set.
            uniform uint _FetchDeliver, _FetchSeq, _FetchLength, _FetchStatus;
            uniform uint _FetchInfo, _FetchW, _FetchH;   // a picture's size: one word, or its halves
            uniform uint _FetchOffset;   // _FetchDeliver 3: where in the file a picture carries this part begins
            Texture2D<float4> _HostData;
            Texture2D<float4> _HostImage;
            // The sound card (docs/machine/sound.md): the sample the host's ring starts at this frame,
            // whether it mixed (the device's words move only then), and its output rate.
            uniform uint _SoundCursor, _SoundMixed, _SoundRate;
            // The network (docs/system/lan.md): the guest's packets the host has taken, the packets it
            // delivered before this frame and how many arrive in it (rows of _NetData, 160 x 4,
            // four bytes a texel), and the machine's number (0: no link).
            uniform uint _NetTxAck, _NetRxSeq, _NetRxCount, _NetId;
            Texture2D<float4> _NetData;
            #define GPU_NET
            #define GPU_SOUND
            #define GPU_STATE _SelfTexture2D
            #define GPU_INPUT
            #include "src/gpu.h"

            // From pixel _ZoneSplit (0: none) on, the row drawn is a second zone at (_Zone2X, _Zone2Y): the control
            // words and the cores' stats (MC_STATS_ALL) in one draw (the harness's zonePair)
            uniform uint _ZoneSplit, _Zone2X, _Zone2Y;
            uint4 frag(v2f_customrendertexture i) : SV_Target {
                uint2 dim;
                _SelfTexture2D.GetDimensions(dim.x, dim.y);
                uint2 p = uint2(i.globalTexcoord.xy * dim);
                if (_ZoneSplit != 0 && p.x >= _ZoneSplit) p = uint2(p.x - _ZoneSplit + _Zone2X, _Zone2Y);
                return gpu_control(p);
            }
            ENDCG
        }

        Pass
        {
            // The picture's depth at far (the harness clears the whole target's to the near plane first): past the picture a
            // depth-tested pass's fragment fails the depth test before it is shaded, as 3D triangles past a program's view
            // were drawn and shaded over the rest of the 2048 square target (docs/machine/gpu.md, "The picture's rectangle").
            // No clip: a pixel inside is drawn as before. Two triangles of the picture's rectangle, depth only.
            Name "GPUClear"
            ZTest Always
            ZWrite On

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            #define GPU_STATE _State
            #include "src/gpu_draw.h"

            float4 vert(uint id : SV_VertexID) : SV_Position {
                uint k = id % 6;
                bool right = k == 1 || k == 2 || k == 4, low = k == 2 || k == 4 || k == 5;
                float2 size = gpu_made_picture();
                return place_pixels(float2(right ? size.x : 0.0, low ? size.y : 0.0), 1.0);
            }
            float4 frag() : SV_Target {
                return 0;
            }
            ENDCG
        }
    }
}
