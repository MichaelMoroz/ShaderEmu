Shader "ShaderEmu/Rain"
{
    // Rain outside the window: sheets of falling streaks at a few distances (world/city.py),
    // added to what is behind them. A vertex's colour is how bright its sheet is; lightning
    // (EmuWeather) shows every drop.
    Properties
    {
        _MainTex ("Streaks", 2D) = "black" {}
        _Speed ("Fall (uv a second)", Float) = 1.4
        _Bright ("Brightness", Float) = 0.05
    }
    SubShader
    {
        Tags { "Queue"="Transparent-100" "RenderType"="Transparent" "IgnoreProjector"="True" }
        Blend One One
        ZWrite Off
        Cull Off

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _MainTex;
            float _Speed, _Bright;
            float _UdonWeatherFlash;

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
                float t = _Time.y;
                float a = tex2D(_MainTex, i.uv + float2(0.03 * t, _Speed * t)).r;
                float b = tex2D(_MainTex, i.uv * 1.7 + float2(0.37 - 0.02 * t, _Speed * 1.3 * t)).r;
                float3 c = (a + 0.6 * b) * (_Bright + _UdonWeatherFlash * 0.9) * i.colour.rgb;
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
