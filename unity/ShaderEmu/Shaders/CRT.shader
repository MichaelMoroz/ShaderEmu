Shader "ShaderEmu/CRT"
{
    // A station's tube (docs/stations.md). The surface is glass: dark, smooth, lit and reflecting
    // as anything in the room is (the room's probe, a little blurred; lamps' highlights). Behind
    // it the picture of DisplayShow.shader glows, drawn by a beam in lines on stripes of three
    // phosphors. Lines and stripes are waves of mean one, each weakened by how much of its period
    // a screen pixel spans: close up they are seen, further off they are their mean, and there is
    // no moire between.
    Properties
    {
        _MainTex ("Display", 2D) = "black" {}
        _TexSize ("Size of that texture", Vector) = (2048, 1024, 0, 0)
        _Size ("Size of the picture in it (0: off)", Vector) = (0, 0, 0, 0)
        _Aspect ("Width / height of the tube", Float) = 1.3333333
        _Glow ("Brightness (above 1 it blooms)", Float) = 1.5
        _Off ("The phosphor unlit, behind the glass", Color) = (0.045, 0.05, 0.047, 1)
        _Glossiness ("The glass's smoothness", Range(0, 1)) = 0.86
        _Triads ("Stripes of three phosphors across the tube", Float) = 520
        _Mask ("How deep the stripes are", Range(0, 1)) = 0.8
        _Scan ("How deep the lines are", Range(0, 1)) = 0.55
        _Curve ("The picture's bulge on a flat face (0: the mesh bulges itself)", Range(0, 0.3)) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }
        CGPROGRAM
        #pragma surface surf Standard fullforwardshadows
        #pragma target 3.5

        sampler2D _MainTex;
        float4 _TexSize, _Size, _Off;
        float _Aspect, _Glow, _Glossiness, _Triads, _Mask, _Scan, _Curve;

        struct Input { float2 uv_MainTex; };

        // A wave of mean one seen through a pixel `span` periods wide (a Gaussian's share of it).
        float wave(float phase, float depth, float span) {
            return 1.0 + depth * cos(6.2831853 * phase) * exp(-4.93 * span * span);
        }

        void surf(Input i, inout SurfaceOutputStandard o)
        {
            float2 p = float2(i.uv_MainTex.x, 1.0 - i.uv_MainTex.y) - 0.5;
            p *= 1.0 + _Curve * dot(p, p) * 4.0 - _Curve * 0.5;
            float2 edge = (0.5 - abs(p)) / max(fwidth(p), 1e-6);
            float inside = saturate(min(edge.x, edge.y) + 0.5);   // the picture's own edge, a pixel wide

            float2 size = max(_Size.xy, 1.0);
            float shape = size.x / size.y;
            float2 q = (p * float2(max(1.0, _Aspect / shape), max(1.0, shape / _Aspect)) + 0.5) * size;
            float2 dx = ddx(q), dy = ddy(q);
            float2 scale = float2(1.0, -1.0) / _TexSize.xy;   // row 0 is the texture's top
            float3 c = tex2Dgrad(_MainTex, float2(0.0, 1.0) + clamp(q, 0.5, size - 0.5) * scale, dx * scale, dy * scale).rgb;
            float2 lit = saturate(min(q, size - q) / max(abs(dx) + abs(dy), 1e-6) + 0.5);
            c *= lit.x * lit.y * step(1.0, _Size.x);

            // the beam's lines, one a row of the picture; the stripes, three colours a triad
            c *= wave(q.y, _Scan, abs(dx.y) + abs(dy.y));
            float at = (p.x + 0.5) * _Triads, span = fwidth(at);
            c *= float3(wave(at, _Mask, span), wave(at - 1.0 / 3.0, _Mask, span), wave(at - 2.0 / 3.0, _Mask, span));
            c *= 1.0 - 0.35 * dot(p, p) * 2.0;   // dimmer towards the corners

            o.Albedo = _Off.rgb;
            o.Metallic = 0;
            o.Smoothness = _Glossiness;
            o.Emission = c * _Glow * inside;
            o.Alpha = 1;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
