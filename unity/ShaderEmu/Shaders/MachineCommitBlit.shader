Shader "ShaderEmu/MachineCommitBlit"
{
    // MachineGather.shader and MachineCommitPoints.shader as blits, with no camera (docs/machine/gpu.md, "Blits in
    // place of cameras"; src/amp.cginc): a blit's two triangles are two patches, tessellated at factor 8 into points,
    // and a geometry shader's instances are calls that make the meshes' points in the meshes' order. Pass 0 is the
    // gather into the stage, a point a call (_GatherPoints of them); pass 1 the RAM commit into the state, nine points
    // a call and the picture's alone (_CommitPoints), four blits of 1,296 calls (_AmpFirst). EmuMachine.Commit blits
    // them around Machine.shader's Commit.
    Properties
    {
        _Ram ("The state (the gather's)", 2D) = "black" {}
        _TickState ("The state rows after the tick", 2D) = "black" {}
        _Stage ("The commit's stage (pass 0 draws it)", 2D) = "black" {}
        _GpuTarget ("The GPU's colour target", 2D) = "black" {}
        _Data_RAM_R ("RAM Texture R", 2D) = "black" {}
        _Data_RAM_G ("RAM Texture G", 2D) = "black" {}
        _Data_RAM_B ("RAM Texture B", 2D) = "black" {}
        _Data_RAM_A ("RAM Texture A", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
        _AmpRank ("Each patch point's place in the card's order (BlitOrder.shader)", 2D) = "black" {}
        _GatherPoints ("The gather's points: 1 + CORES", Int) = 257
        _CommitPoints ("The RAM commit's points", Int) = 41346
        _FieldPoints ("Of them the lanes' field's buckets, before the picture's", Int) = 7936
        _AmpFirst ("The first call of this blit (pass 1: four blits)", Int) = 0
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
            Name "Gather"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry ram_gather_amp_geom
            #pragma fragment frag
            #pragma editor_sync_compilation

            #include "src/fxc2_check.cginc"
            #define PASS_COMMIT
            #define GPU_DEVICE
            #define COMMIT_GATHER
            #define RAM_AMP
            #define AMP_INST 2   // 324 calls, a point each: 257 points

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"
            #include "MachineCommit.cginc"

            uint4 frag(ram_v2f i) : SV_Target {
                return ram_gather_texel(i);
            }
            ENDCG
        }

        Pass
        {
            Name "Points"

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry ram_commit_amp_geom
            #pragma fragment frag
            #pragma editor_sync_compilation

            #include "src/fxc2_check.cginc"
            #define PASS_COMMIT
            #define GPU_DEVICE
            #define COMMIT_RAM
            #define RAM_AMP
            #define AMP_INST 8   // 1,296 calls a blit, nine points each: four blits (_AmpFirst)

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"
            #include "MachineCommit.cginc"

            uint4 frag(ram_v2f i) : SV_Target {
                return ram_commit_texel(i);
            }
            ENDCG
        }
    }
}
