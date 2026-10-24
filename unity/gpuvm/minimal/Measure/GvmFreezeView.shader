Shader "Hidden/GvmFreezeView"
{
    // (a measure: GvmProbe's "frozen") GvmFreeze's row 0 as colours a probe can read: r frozen, g quiet / 24
    SubShader
    {
        Cull Off ZTest Always ZWrite Off
        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert_img
            #pragma fragment f
            #include "UnityCG.cginc"
            Texture2D<uint4> _F;
            float4 f(v2f_img i) : SV_Target {
                uint4 v = _F.Load(int3(uint(i.pos.x), 0, 0));
                return float4(v.x != 0 ? 1 : 0, min(v.y, 24u) / 24.0, v.z != 0 ? 1 : 0, (v.w & 7) / 7.0);
            }
            ENDCG
        }
    }
}
