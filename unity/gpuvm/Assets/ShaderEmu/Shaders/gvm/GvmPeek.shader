Shader "Hidden/ShaderEmu/GvmPeek"
{
    // An editor's look at gpu_vm's RAM (a debugging aid, never drawn by the world): row y of the target is the texels
    // from texel _Peek[y] on, a pixel a word, bytes in r, g, b, a.
    Properties { _Ram ("RAM", 2D) = "black" {} }
    SubShader
    {
        Cull Off ZTest Always ZWrite Off
        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            Texture2D<uint4> _Ram;
            float4 _Peek[16];   // texel numbers (exact below 2^24)
            float4 vert(appdata_img v) : SV_Position { return UnityObjectToClipPos(v.vertex); }
            float4 frag(float4 at : SV_Position) : SV_Target {
                uint x = (uint)at.x, y = (uint)at.y, t = (uint)_Peek[y].x + x / 4, k = x & 3;
                uint4 q = _Ram[uint2(t % 8192, t / 8192)];
                uint v = k == 0 ? q.r : k == 1 ? q.g : k == 2 ? q.b : q.a;
                return float4(v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v >> 24) / 255.0;
            }
            ENDCG
        }
    }
}
