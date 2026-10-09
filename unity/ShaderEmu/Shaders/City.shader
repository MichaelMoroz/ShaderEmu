Shader "ShaderEmu/City"
{
    // The city outside the window: unlit geometry with a texture of lit windows. A vertex's
    // colour shades its face, and its alpha is how far the haze has swallowed it.
    Properties
    {
        _MainTex ("Texture", 2D) = "black" {}
        _Haze ("Haze", Color) = (0.035, 0.032, 0.045, 1)
        _Dark ("Unlit surfaces", Float) = 0.6
        _Bright ("Lights (above 1 they bloom)", Float) = 5
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
            float _Dark, _Bright;
            float _UdonWeatherFlash;   // lightning (EmuWeather)

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
                // a lit window is a lamp: many times brighter than the wall round it
                float3 lit = c * _UdonWeatherFlash * 2.5;
                c = c * lerp(_Dark, _Bright, smoothstep(0.12, 0.7, max(c.r, max(c.g, c.b)))) + lit;
                return float4(lerp(c, _Haze.rgb * (1.0 + 10.0 * _UdonWeatherFlash), i.colour.a), 1);
            }
            ENDCG
        }
    }
}
