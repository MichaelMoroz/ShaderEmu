Shader "ShaderEmu/MemView"
{
    // Shows the memory view's render texture (MemHeat.shader): RAM as colours, writes glowing.
    Properties
    {
        _Heat ("Memory heat texture", 2D) = "black" {}
        _Strips ("Strips RAM is cut into (as MemHeat's)", Int) = 2
        _Cursor ("The danger zone's laser: u, v, reach (of u), firing", Vector) = (-1, -1, 0, 0)
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
            float4 _Cursor;

            // Whose a megabyte of RAM is, as a colour for the strip's left edge: the devices' first
            // (window buffers, the display and the GPU's lists and textures, a 3D program's, the sound
            // card's, the device words), then Linux's own, the kernel brighter (docs/machine/gpu.md,
            // docs/machine/sound.md, docs/system/nommu.md "Memory"; src/zap.h's zap_zone).
            float3 owner(float megabyte) {
                if (megabyte < 16.0) return float3(0.30, 0.80, 0.40);
                if (megabyte < 23.0) return float3(0.95, 0.80, 0.25);
                if (megabyte < 27.0) return float3(0.75, 0.45, 0.95);
                if (megabyte < 30.0) return float3(1.00, 0.55, 0.20);
                if (megabyte < 32.0) return float3(0.10, 0.11, 0.14);
                if (megabyte < 41.0) return float3(0.35, 0.60, 1.00);
                return float3(0.16, 0.26, 0.45);
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
                if (across - strip < 0.022) c = owner((strip + 1.0 - i.uv.y) / strips * 288.0);
                // the laser: a square round where it points (as many texels in v as in u: a strip
                // is 9,216 / strips rows of 2,048), never too small to see
                if (_Cursor.x >= 0.0) {
                    float2 reach = max(float2(_Cursor.z, _Cursor.z * strips * strips * 2048.0 / 9216.0), float2(0.004, 0.008));
                    float2 off = abs(i.uv - _Cursor.xy) / reach;
                    float edge = max(off.x, off.y);
                    if (edge < 1.0 && edge > 0.72) c = _Cursor.w > 0.5 ? float3(1.0, 0.25, 0.1) : float3(1.0, 1.0, 1.0);
                }
#ifndef UNITY_COLORSPACE_GAMMA
                c = GammaToLinearSpace(c);
#endif
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
