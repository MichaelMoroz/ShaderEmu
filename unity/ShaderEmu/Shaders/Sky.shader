Shader "ShaderEmu/Sky"
{
    // The night sky as a skybox: a panorama (world/textures.py: u = 0.5 looks along +x, v = 1
    // straight up) under a layer of cloud that drifts and that lightning lights (EmuWeather).
    Properties
    {
        _MainTex ("Panorama", 2D) = "black" {}
        _Clouds ("Cloud noise", 2D) = "grey" {}
        _Exposure ("Exposure", Float) = 0.5
        _Cover ("Cloud cover", Range(0, 1)) = 0.8
        _Wind ("Drift (uv a second)", Vector) = (0.004, 0.0015, 0, 0)
    }
    SubShader
    {
        Tags { "Queue"="Background" "RenderType"="Background" "PreviewType"="Skybox" }
        Cull Off ZWrite Off

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            sampler2D _MainTex, _Clouds;
            float _Exposure, _Cover;
            float4 _Wind;
            float _UdonWeatherFlash;

            struct v2f {
                float4 pos : SV_Position;
                float3 dir : TEXCOORD0;
            };

            v2f vert(appdata_base v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.dir = v.vertex.xyz;
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                float3 d = normalize(i.dir);
                float2 uv = float2(0.5 - atan2(d.z, d.x) / 6.2831853, 1.0 - acos(clamp(d.y, -1.0, 1.0)) / 3.14159265);
                float3 c = tex2Dlod(_MainTex, float4(uv, 0, 0)).rgb * _Exposure;
                if (d.y > 0.0)
                {
                    // the cloud is a flat layer overhead: far more of it towards the horizon
                    float2 p = d.xz / (d.y + 0.12);
                    float t = _Time.y;
                    float n = tex2D(_Clouds, p * 0.06 + _Wind.xy * t).r * 0.65 + tex2D(_Clouds, p * 0.17 - _Wind.yx * t * 1.7).r * 0.35;
                    float cover = smoothstep(0.75 - _Cover, 1.25 - _Cover, n);
                    // lit from below by the city, most where the cloud is seen edge on
                    float3 cloud = lerp(float3(0.030, 0.027, 0.034), float3(0.150, 0.095, 0.070), saturate(1.0 - d.y * 2.2)) * (0.55 + 0.9 * n);
                    cloud += _UdonWeatherFlash * float3(1.6, 1.75, 2.2) * (0.25 + n) * saturate(0.55 + 0.45 * dot(normalize(d.xz), float2(0.96, 0.28)));
                    c = lerp(c, cloud, cover);
                }
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
