Shader "ShaderEmu/MachineGather"
{
    // The commit's gather (src/ram.cginc, docs/machine/gpu.md "RAM in one texture"): what the
    // RAM commit needs of RAM as it was, read from the state into the stage before anything is
    // written there: a copy's chunk, the texels a core's lone store or a fill's ends share with
    // others, the control words, the picture's and the zap's part texels. Points drawn by the
    // points camera into the stage (EmuMachine.Commit), point 0 and then one a core.
    Properties
    {
        _Ram ("The state", 2D) = "black" {}
        _TickState ("The state rows after the tick", 2D) = "black" {}
        _GpuTarget ("The GPU's colour target", 2D) = "black" {}
        _Data_ROM ("ROM, a 16-byte texel a pixel (RGBAFloat, read by its bits)", 2D) = "black" {}
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
            #pragma vertex vert
            #pragma geometry ram_gather_geom
            #pragma fragment frag
            #pragma editor_sync_compilation

            #define PASS_COMMIT
            #define GPU_DEVICE
            #define COMMIT_GATHER
            // only the points camera draws them, into the stage (its target's size says which that is)
            #define RAM_GEOM_GUARD if ((uint)_ScreenParams.x != STAGE_W || (uint)_ScreenParams.y != STAGE_ROWS) return;

            #include "UnityCG.cginc"
            #include "MachineBlit.cginc"
            #include "MachineCommit.cginc"

            ram_point vert(appdata_base v, uint id : SV_VertexID) {
                return ram_point_vert(id);
            }

            uint4 frag(ram_v2f i) : SV_Target {
                return ram_gather_texel(i);
            }
            ENDCG
        }
    }
}
