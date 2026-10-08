Shader "ShaderEmu/MemView"
{
    // Shows the memory view's render texture (MemHeat.shader): RAM as colours, writes glowing.
    Properties
    {
        _Heat ("Memory heat texture", 2D) = "black" {}
        _Strips ("Strips RAM is cut into (as MemHeat's)", Int) = 2
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
            uint _Strips;

            // Whose a megabyte of RAM is, as a colour for the strip's left edge: Linux's own
            // (the kernel brighter), window buffers, the display and the GPU's lists and
            // textures, a 3D program's, the sound card's (docs/gpu.md, docs/sound.md).
            float3 owner(float megabyte) {
                if (megabyte < 4.0) return float3(0.10, 0.11, 0.14);
                if (megabyte < 9.0) return float3(0.35, 0.60, 1.00);
                if (megabyte < 96.0) return float3(0.16, 0.26, 0.45);
                if (megabyte < 112.0) return float3(0.30, 0.80, 0.40);
                if (megabyte < 119.0) return float3(0.95, 0.80, 0.25);
                if (megabyte < 123.0) return float3(0.75, 0.45, 0.95);
                return float3(1.00, 0.55, 0.20);
            }

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
                // the legend: each strip's left edge says whose that memory is
                float strips = (float)max(_Strips, 1u);
                float across = i.uv.x * strips, strip = floor(across);
                if (across - strip < 0.022) c = owner((strip + 1.0 - i.uv.y) / strips * 126.0);
#ifndef UNITY_COLORSPACE_GAMMA
                c = GammaToLinearSpace(c);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
