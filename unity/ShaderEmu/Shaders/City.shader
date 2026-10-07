Shader "ShaderEmu/City"
{
    // The city outside the window: unlit geometry with a texture of lit windows. A vertex's
    // colour shades its face, and its alpha is how far the haze has swallowed it.
    Properties
    {
        _MainTex ("Texture", 2D) = "black" {}
        _Haze ("Haze", Color) = (0.12, 0.09, 0.10, 1)
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
            float4 _Haze;

            struct v2f {
                float4 pos : SV_Position;
                float2 uv : TEXCOORD0;
                float4 colour : COLOR;
            };

            v2f vert(appdata_full v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.texcoord.xy;
                o.colour = v.color;
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                float3 c = tex2D(_MainTex, i.uv).rgb * i.colour.rgb;
                return float4(lerp(c, _Haze.rgb, i.colour.a), 1);
            }
            ENDCG
        }
    }
}
