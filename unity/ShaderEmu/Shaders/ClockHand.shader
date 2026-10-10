Shader "ShaderEmu/ClockHand"
{
    // A clock's hand (docs/world.md): a mesh that points at twelve, turned about its own z by the
    // time of day. _UdonClockDay is the visitor's own clock in seconds when the world began
    // (EmuWeather sets it); with _Time it is now.
    Properties
    {
        _MainTex ("Colour", 2D) = "white" {}
        _BumpMap ("Normals", 2D) = "bump" {}
        _Metallic ("Metallic", Range(0, 1)) = 0
        _Glossiness ("Smoothness", Range(0, 1)) = 0.4
        _Period ("Seconds a turn", Float) = 60
        _Steps ("Steps a turn (0: it sweeps)", Float) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }
        CGPROGRAM
        #pragma surface surf Standard vertex:vert addshadow
        #pragma target 3.0

        sampler2D _MainTex, _BumpMap;
        float _Metallic, _Glossiness, _Period, _Steps, _UdonClockDay;

        struct Input { float2 uv_MainTex; };

        void vert(inout appdata_full v) {
            float turn = frac((_UdonClockDay + _Time.y) / _Period);
            if (_Steps > 0.5) turn = floor(turn * _Steps) / _Steps;
            float s, c;
            // clockwise to whoever faces the dial, which looks along the hand's -z
            sincos(turn * 6.2831853, s, c);
            v.vertex.xy = float2(v.vertex.x * c - v.vertex.y * s, v.vertex.x * s + v.vertex.y * c);
            v.normal.xy = float2(v.normal.x * c - v.normal.y * s, v.normal.x * s + v.normal.y * c);
            v.tangent.xy = float2(v.tangent.x * c - v.tangent.y * s, v.tangent.x * s + v.tangent.y * c);
        }

        void surf(Input i, inout SurfaceOutputStandard o) {
            o.Albedo = tex2D(_MainTex, i.uv_MainTex).rgb;
            o.Normal = UnpackNormal(tex2D(_BumpMap, i.uv_MainTex));
            o.Metallic = _Metallic;
            o.Smoothness = _Glossiness;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
