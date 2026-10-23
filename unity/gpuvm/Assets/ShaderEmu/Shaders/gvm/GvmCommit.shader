Shader "ShaderEmu/GvmCommit"
{
    // The tick's commit: a point a write cache entry into RAM, four passes, a word of the texel each (the harness's
    // write masks); a core from _NCores on draws nothing (its row is not this pass's).
    Properties
    {
        _S0 ("State 0", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend Off
        CGINCLUDE
        #pragma target 5.0
        #pragma vertex gvm_commit_vert
        #pragma fragment commit_frag
        ENDCG
        Pass
        {
            ColorMask R
            CGPROGRAM
            #define _Comp 0u
            #include "commit.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask G
            CGPROGRAM
            #define _Comp 1u
            #include "commit.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask B
            CGPROGRAM
            #define _Comp 2u
            #include "commit.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask A
            CGPROGRAM
            #define _Comp 3u
            #include "commit.cginc"
            ENDCG
        }
    }
}
