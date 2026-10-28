Shader "ShaderEmu/MachineCommitPoints"
{
    // The commit of RAM, written in place (src/ram.cginc, docs/machine/gpu.md "RAM in one texture";
    // main.shader's COMMIT_RAM), drawn as points by the points camera into the state after the
    // gather (MachineGather.shader): a geometry shader makes each point the rectangle it stands
    // for. Point 0 is the zap's square, or all of RAM while the machine starts; then each core's
    // buckets, lone store and fill or copy (258 core 0's, 130 a worker's); the lanes' field's buckets
    // (7,936: docs/machine/lanes.md); the last the picture.
    Properties
    {
        _TickState ("The state rows after the tick", 2D) = "black" {}
        _Stage ("The commit's stage (MachineGather.shader)", 2D) = "black" {}
        _GpuTarget ("The GPU's colour target", 2D) = "black" {}
        _Data_RAM_R ("RAM Texture R", 2D) = "black" {}
        _Data_RAM_G ("RAM Texture G", 2D) = "black" {}
        _Data_RAM_B ("RAM Texture B", 2D) = "black" {}
        _Data_RAM_A ("RAM Texture A", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _CommitPoints ("Points the mesh has", Int) = 41346
        _FieldPoints ("Of them the lanes' field's buckets, before the picture's", Int) = 7936
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "CommitPoints"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry ram_commit_geom
            #pragma fragment frag
            #pragma editor_sync_compilation

            #define PASS_COMMIT
            #define GPU_DEVICE
            #define COMMIT_RAM
            // only the points camera draws them, into the state (its target's size says which that is)
            #define RAM_GEOM_GUARD if ((uint)_ScreenParams.x != 2048u || (uint)_ScreenParams.y != STATE_TEX_HEIGHT) return;

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"
            #include "MachineCommit.cginc"

            ram_point vert(appdata_base v, uint id : SV_VertexID) {
                return ram_point_vert(id);
            }

            uint4 frag(ram_v2f i) : SV_Target {
                return ram_commit_texel(i);
            }
            ENDCG
        }
    }
}
