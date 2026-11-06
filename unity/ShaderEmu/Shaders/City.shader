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
        _LampBright ("Lights on a carrier (world/city.py's box(light=...)): their light, times their colour", Float) = 80
        _Housing ("Lights on a carrier: the housing, times the light's colour", Float) = 0.04
        _FogColour ("Height fog: its colour", Color) = (0.167, 0.167, 0.177, 1)
        _FogFlash ("Height fog: lit by lightning (at a flash of 1, ten times this)", Color) = (0.05, 0.05, 0.06, 1)
        _FogDensity ("Height fog: a metre's at its base", Float) = 0.125
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
            float _Dark, _Bright, _Night, _LampBright, _Housing;

            // the share of a pixel spanning [x - w/2, x + w/2] that [a, b] covers (a box filter: exact for a
            // line, and as thin as it likes: under a pixel it is its width over the pixel's, the same light)
            float Covered(float x, float w, float a, float b) {
                w = max(w, 1e-5);
                return saturate((min(x + 0.5 * w, b) - max(x - 0.5 * w, a)) / w);
            }

            // A light drawn on a carrier's face (city.py's box(light=...)): u's whole part its kind (100 a
            // lamp, 200 a line, 300 a line in strokes) and size (per cent of the face), its fraction 0.05 to
            // 0.95 across the face; v 0 to 1 up it. What it gives is its light's share of the pixel, at
            // 1 where the light fills it: thinner than the pixel it is less, never gone between pixels.
            float CarriedLight(float2 uv) {
                float code = floor(uv.x);
                float kind = floor(code / 100.0), size = (code - 100.0 * kind) / 100.0;
                float2 f = float2((frac(uv.x) - 0.05) / 0.9, uv.y);   // 0 to 1 across and up the face
                float2 w = fwidth(f);
                if (kind == 1)   // a lamp `size` of the face round: a gaussian as wide as it and the pixel together, the same light
                {
                    float r = 0.5 * size;
                    float s2 = r * r + 0.25 * dot(w, w);
                    float2 d = f - 0.5;
                    return (r * r / s2) * exp(-0.5 * dot(d, d) / s2);
                }
                float band = Covered(f.y, w.y, 0.5 - 0.5 * size, 0.5 + 0.5 * size);
                if (kind == 2) return band;
                // in strokes: 7 a face, two in three lit, each 80% of its cell; far off, their mean
                const float cells = 7.0;
                float cell = floor(f.x * cells);
                float on = frac(sin(cell * 91.7 + code * 1.3) * 43758.5453) < 0.67 ? 1.0 : 0.0;
                float x = frac(f.x * cells), wx = w.x * cells;
                float stroke = on * Covered(x, wx, 0.1, 0.9);
                return band * lerp(stroke, 0.67 * 0.8, smoothstep(0.3, 0.8, wx));
            }
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
                if (i.uv.x >= 100.0)   // a carrier: its dark housing and the light on it (lightning leaves it)
                {
                    c = i.colour.rgb * (_Housing + _LampBright * CarriedLight(i.uv));
                    lit = 0;
                }
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
