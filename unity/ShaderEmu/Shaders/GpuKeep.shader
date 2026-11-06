Shader "ShaderEmu/GpuKeep"
{
    // What the GPU drew this frame, kept apart for those that must not see every list (EmuMachine.GpuDraw, after the passes;
    // docs/world/volume.md, "What the host draws"). The front end's targets and the draw IDs are every list's, the window
    // system's too: the holodeck draws a 3D program's whole frame, so its copies (passes 0 and 1) are taken only in a frame
    // whose list is the one the volume words name (0x81000300), and kept otherwise. A list drawn SUBMIT_SIZED is a picture
    // only for its copies into the VRAM: the display's copy of the target (pass 2) is taken only from a list drawn to be seen.
    // The VRAM's scratch band is kept with the holodeck's copies (pass 3): every list's textures are copied there before its
    // passes, the desktop's over the 3D program's. Each is a blit of the source (_MainTex) that discards where it keeps the old.
    Properties
    {
        _MainTex ("The source (the blit's)", 2D) = "black" {}
        _State ("Machine state texture", 2D) = "black" {}
        _StateRow ("The state texture's row RAM starts in", Float) = 64
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
    }
    CGINCLUDE
    #include "UnityCG.cginc"
    Texture2D<uint4> _State;
    Texture2D<float4> _Data_ROM;   // (gpu_draw's readers may take a word from the ROM; nothing here does)
    #define GPU_ROM
    #define GPU_STATE _State
    float _StateRow;
    #define GPU_STATE_ROW ((uint)(int)_StateRow)
    #define GPU_TARGET_W 1280.0
    #define GPU_TARGET_H 720.0
    #include "src/gpu_draw.cginc"

    float4 vert(float4 v : POSITION) : SV_Position { return float4(v.xy * 2.0 - 1.0, 0.5, 1.0); }

    // a list was drawn this frame, and it is the frame the volume words name
    bool keep_volume() {
        uint list = ram(VOLUME_LIST).r;
        return gpu_mesh_used() != 0 && list != 0 && ram(GPU_CTRL).g == list;
    }
    // a list was drawn this frame, to be seen
    bool keep_shown() {
        return gpu_mesh_used() != 0 && (ram(GPU_CTRL).r & SUBMIT_SIZED) == 0;
    }
    ENDCG
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Blend One Zero
        Pass
        {
            // 0: a float target of the front end's, for the holodeck
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<float4> _MainTex;
            float4 frag(float4 p : SV_Position) : SV_Target {
                if (!keep_volume()) discard;
                return _MainTex.Load(int3(p.xy, 0));
            }
            ENDCG
        }
        Pass
        {
            // 1: an integer target (the front end's two of words, the draw IDs), for the holodeck
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<int4> _MainTex;
            int4 frag(float4 p : SV_Position) : SV_Target {
                if (!keep_volume()) discard;
                return _MainTex.Load(int3(p.xy, 0));
            }
            ENDCG
        }
        Pass
        {
            // 2: the GPU's target, for the display
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<float4> _MainTex;
            float4 frag(float4 p : SV_Position) : SV_Target {
                if (!keep_shown()) discard;
                return _MainTex.Load(int3(p.xy, 0));
            }
            ENDCG
        }
        Pass
        {
            // 3: the VRAM's scratch band (its rows 2048 to 4095, level 0), for the holodeck: the next list of any program
            // copies its textures over the kept frame's (src/gpu_draw.h, GPU_KEPT_SCRATCH)
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            Texture2D<float4> _MainTex;
            float4 frag(float4 p : SV_Position) : SV_Target {
                if (!keep_volume()) discard;
                return _MainTex.Load(int3(p.x, p.y + 2048.0, 0));
            }
            ENDCG
        }
    }
}
