Shader "ShaderEmu/GpuFront"
{
    // The GPU's vertex front end (docs/machine/gpu.md, "The vertex front end"; shader_vm/gpu.shader's GPUVertices): every vertex
    // the list's commands claim made once, from the list in RAM, into EmuMachine's eight targets of 512 x 385 (its camera's
    // SetTargetBuffers), after the draw IDs and before the uploads; the row past the mesh's is the frame's, for the holodeck.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _StateRow ("The state texture's row RAM starts in", Float) = 64
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Pass
        {
            Name "GpuFront"
            Cull Off
            ZTest Always
            ZWrite Off
            Blend One Zero

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            Texture2D<uint4> _State;
            Texture2D<float4> _Data_ROM;   // (packed vertices and glyph tables may be a file's bytes where it lies in the ROM)
            #define GPU_ROM
            #define GPU_STATE _State
            float _StateRow;
            #define GPU_STATE_ROW ((uint)(int)_StateRow)
            #define GPU_TARGET_W 1280.0
            #define GPU_TARGET_H 720.0
            #include "src/gpu_draw.cginc"

            // the rows the list's vertices take and the frame's row (gpu_rows_vertex: the mesh's twelve vertices)
            float4 vert(uint id : SV_VertexID) : SV_Position {
                return gpu_rows_vertex(id, gpu_used_rows(), GPU_DRAW_IDS_H + 1);
            }
            gpu_made frag(float4 p : SV_Position) {
                uint2 at = uint2(p.xy);
                return gpu_front_fragment(at.y * GPU_DRAW_IDS_W + at.x);
            }
            ENDCG
        }
    }
}
