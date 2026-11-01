Shader "ShaderEmu/AmpOrder"
{
    // The blits' order probe (src/amp.h, docs/machine/gpu.md "Blits in place of cameras"), as the harness draws it
    // with --blits: AmpOrder is a blit's two patches into an 81 x 81 target, AmpRank a triangle over an 81 x 1 one
    // that reads it. Unity's world has the same two passes in BlitOrder.shader.
    Properties
    {
        _AmpOrder ("The probe's 81 x 81 target", 2D) = "black" {}
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
            Name "AmpOrder"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex amp_vert
            #pragma hull amp_hull
            #pragma domain amp_dom
            #pragma geometry amp_order_geom
            #pragma fragment amp_order_frag
            #include "src/fxc2_check.h"
            #define AMP_PROBE
            #include "src/amp.h"
            ENDCG
        }
        Pass
        {
            Name "AmpRank"
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "src/fxc2_check.h"
            #define AMP_PROBE
            #include "src/amp.h"
            float4 vert(uint id : SV_VertexID) : SV_Position {
                return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.5, 1.0);
            }
            float4 frag(float4 pos : SV_Position) : SV_Target {
                return amp_rank_of((uint)pos.x);
            }
            ENDCG
        }
    }
}
