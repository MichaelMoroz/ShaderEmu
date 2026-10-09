Shader "ShaderEmu/DisplayShow"
{
    // The display on the wall. _MainTex holds the picture a pixel a texel from its top left
    // corner, with mipmaps. Close up each pixel is sharp with an edge one screen pixel wide;
    // further away it turns into ordinary filtering. EmuPointer.cs fits the pointer the same way.
    Properties
    {
        _MainTex ("Decoded display", 2D) = "black" {}
        _TexSize ("Size of that texture", Vector) = (2048, 1024, 0, 0)
        _Size ("Size of the picture in it (0: off)", Vector) = (0, 0, 0, 0)
        _Aspect ("Width / height of the quad", Float) = 1.7777778
        _Glow ("Brightness (above 1 it blooms)", Float) = 1
        _Off ("Colour while the display is off", Color) = (0.01, 0.011, 0.014, 1)
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float4 _TexSize, _Size, _Off;
            float _Aspect;
            float _Glow;

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
                float2 size = max(_Size.xy, 1.0);
                float shape = size.x / size.y;
                // centred, as large as fits
                float2 p = (float2(i.uv.x, 1.0 - i.uv.y) - 0.5) * float2(max(1.0, _Aspect / shape), max(1.0, shape / _Aspect));
                float2 q = (p + 0.5) * size;
                float2 dx = ddx(q), dy = ddy(q);   // before any branch
                if (_Size.x < 1.0 || q.x < 0 || q.y < 0 || q.x >= size.x || q.y >= size.y) return _Off;

                // Towards the nearest texel boundary by the screen pixel's share of a texel: the
                // bilinear blend then spans one screen pixel, or the whole texel when minified.
                float2 seam = floor(q + 0.5);
                float2 s = seam + clamp((q - seam) / max(abs(dx) + abs(dy), 1e-6), -0.5, 0.5);
                s = clamp(s, 0.5, size - 0.5);
                float2 scale = float2(1.0, -1.0) / _TexSize.xy;   // row 0 is the texture's top
                float3 c = tex2Dgrad(_MainTex, float2(0.0, 1.0) + s * scale, dx * scale, dy * scale).rgb;
                return float4(c * _Glow, 1);
            }
            ENDCG
        }
    }
}
