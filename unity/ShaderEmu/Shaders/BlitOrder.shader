Shader "ShaderEmu/BlitOrder"
{
    // Which order this card's tessellator draws a patch's 81 points in (src/amp.cginc, docs/machine/gpu.md "Blits in
    // place of cameras"): WARP's is not NVIDIA's, and the blits' calls must be drawn in the meshes' order. Pass 0 is a
    // blit into an 81 x 81 target whose texel (i, j) ends as the later of points i and j; pass 1 a blit into an 81 x 1
    // target, each point's place, which MachineCommitBlit and GpuDrawBlit read as _AmpRank. EmuMachine blits both
    // every frame before its rounds.
    Properties
    {
        _AmpOrder ("Pass 0's target", 2D) = "black" {}
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
            Name "Order"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry amp_order_geom
            #pragma fragment amp_order_frag
            #pragma editor_sync_compilation
            #include "src/fxc2_check.cginc"
            #define AMP_PROBE
            #include "src/amp.cginc"
            ENDCG
        }
        Pass
        {
            Name "Rank"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #pragma editor_sync_compilation
            #include "UnityCG.cginc"
            #include "src/fxc2_check.cginc"
            #define AMP_PROBE
            #include "src/amp.cginc"
            float4 vert(float4 v : POSITION) : SV_Position {
                return UnityObjectToClipPos(v);
            }
            float4 frag(float4 pos : SV_Position) : SV_Target {
                return amp_rank_of((uint)pos.x);
            }
            ENDCG
        }
    }
}
