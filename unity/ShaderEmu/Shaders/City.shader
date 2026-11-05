Shader "ShaderEmu/City"
{
    // The city outside the window: unlit geometry with a texture of lit windows. A vertex's
    // colour shades its face, and its alpha is how far the haze has swallowed it;
    // over that a fog that thins with height (HeightFog.cginc), as the sky's.
    Properties
    {
        _MainTex ("Texture", 2D) = "black" {}
        _Haze ("Haze", Color) = (0.035, 0.032, 0.045, 1)
        _HazeFlash ("Haze lit by lightning (at a flash of 1, ten times this)", Color) = (0.035, 0.032, 0.045, 1)
        _Dark ("Unlit surfaces", Float) = 0.6
        _Bright ("Lights (above 1 they bloom)", Float) = 5
        _Night ("The night's own light: what is not a bright light, times this", Float) = 1
        _FogColour ("Height fog: its colour", Color) = (0.08, 0.057, 0.047, 1)
        _FogFlash ("Height fog: lit by lightning (at a flash of 1, ten times this)", Color) = (0.04, 0.036, 0.048, 1)
        _FogDensity ("Height fog: a metre's at its base", Float) = 0.025
        _FogBase ("Height fog: its base (world y, the street)", Float) = -40
        _FogFalloff ("Height fog: metres up for a fall by e", Float) = 20
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
            #include "HeightFog.cginc"

            sampler2D _MainTex;
            float4 _Haze, _HazeFlash;
            float _Dark, _Bright, _Night;
            float _UdonWeatherFlash;   // lightning (EmuWeather)

            struct v2f {
                float4 pos : SV_Position;
                float2 uv : TEXCOORD0;
                float4 colour : COLOR;
                float3 world : TEXCOORD1;
            };

            v2f vert(appdata_full v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.texcoord.xy;
                o.colour = v.color;
                o.world = mul(unity_ObjectToWorld, v.vertex).xyz;
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                float3 c = tex2D(_MainTex, i.uv).rgb * i.colour.rgb;
                // a lit window is a lamp: many times brighter than the wall round it
                float3 lit = c * _UdonWeatherFlash * 2.5;
                c = c * lerp(_Dark, _Bright, smoothstep(0.12, 0.7, max(c.r, max(c.g, c.b))));
                // the night darker but not its lights: what blooms (1 and above) as it was, what is under a
                // quarter of that times _Night, smoothly between (the texture's windows run from dim to
                // bright, and its grey roofs were lit half as lamps: _Dark alone left them). Lightning after
                c = c * lerp(_Night, 1.0, smoothstep(0.25, 1.0, max(c.r, max(c.g, c.b)))) + lit;
                // (the haze lightning lights apart from its own: the night darker, the flash as bright)
                c = lerp(c, _Haze.rgb + _HazeFlash.rgb * 10.0 * _UdonWeatherFlash, i.colour.a);
                // and the fog that lies over the streets, thinner with height (HeightFog.cginc)
                float3 ray = i.world - _WorldSpaceCameraPos;
                float len = length(ray);
                return float4(HeightFogged(c, HeightFogDepth(ray / len, len), _UdonWeatherFlash), 1);
            }
            ENDCG
        }
    }
}
