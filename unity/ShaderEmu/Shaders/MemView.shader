Shader "ShaderEmu/MemView"
{
    // Shows the memory view's render texture (MemHeat.shader): RAM as colours, writes glowing.
    Properties
    {
        _Heat ("Memory heat texture", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }

        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            Texture2D<float4> _Heat;

            struct v2f {
                float4 pos : SV_Position;
                float2 uv : TEXCOORD0;
            };

            v2f vert(appdata_base v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.texcoord.xy;
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                uint2 dim;
                _Heat.GetDimensions(dim.x, dim.y);
                // low addresses at the top
                float4 t = _Heat.Load(int3(min((uint2)(float2(i.uv.x, 1.0 - i.uv.y) * dim), dim - 1), 0));
                uint packed = (uint)t.b;
                float3 base = float3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff) / 255.0 * 0.88 + 0.12;
                if (t.a == 0) base = float3(0.02, 0.025, 0.04);
                float heat = t.g;
                float3 glow = lerp(float3(1.0, 0.45, 0.05), float3(1.0, 0.95, 0.8), saturate(heat * heat));
                float3 c = lerp(base, glow, saturate(heat * 1.2));
#ifndef UNITY_COLORSPACE_GAMMA
                c = GammaToLinearSpace(c);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
