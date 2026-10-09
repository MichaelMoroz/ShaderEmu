Shader "ShaderEmu/Plate"
{
    // A panel's plate, or a keyboard's caps: a lit surface with its lettering printed on it.
    // _Index names the two glyphs nearest a place (0: none), _Glyphs is three texels a glyph (its
    // box here, its box in _Fonts, its colour and weight), _Fonts the fonts' distance fields.
    // Where a pixel holds several of the fields' texels the lettering is _Far, its own picture.
    Properties
    {
        _MainTex ("Surface", 2D) = "white" {}
        _Color ("Tint", Color) = (1, 1, 1, 1)
        [NoScaleOffset] _Index ("Glyphs of each place", 2D) = "black" {}
        [NoScaleOffset] _Glyphs ("Glyphs", 2D) = "black" {}
        [NoScaleOffset] _Fonts ("Fonts' distance fields", 2D) = "black" {}
        [NoScaleOffset] _Far ("The lettering from far off (colour times cover, cover)", 2D) = "black" {}
        _Usual ("The sheet's texels across the plate, for its usual lettering", Vector) = (0, 0, 0, 0)
        _GradientScale ("The fields' gradient scale", Float) = 9
        _Glossiness ("Smoothness", Range(0, 1)) = 0.3
        _InkGlossiness ("The lettering's smoothness", Range(0, 1)) = 0.65
        _InkMetallic ("The lettering's metal", Range(0, 1)) = 0
        _Cutoff ("Cut out below this alpha", Range(0, 1)) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }
        CGPROGRAM
        #pragma surface surf Standard fullforwardshadows
        #pragma target 4.0

        sampler2D _MainTex, _Fonts, _Far, _Index, _Glyphs;   // the last two are point filtered
        float4 _Color, _Glyphs_TexelSize, _Fonts_TexelSize, _Usual;
        float _Glossiness, _InkGlossiness, _InkMetallic, _Cutoff, _GradientScale;

        struct Input { float2 uv_MainTex; };

        float4 glyph_word(int n) { return tex2Dlod(_Glyphs, float4((float2(n & 1023, n >> 10) + 0.5) * _Glyphs_TexelSize.xy, 0, 0)); }

        // How much of a pixel at uv a glyph's ink covers; grain is the uv a pixel spans.
        // size: the fields' texels a pixel spans for this glyph.
        float glyph_cover(int glyph, float2 uv, float2 grain, out float3 colour, out float size)
        {
            int n = (glyph - 1) * 3;
            float4 here = glyph_word(n), there = glyph_word(n + 1), ink = glyph_word(n + 2);
            float2 ratio = (there.zw - there.xy) / (here.zw - here.xy);
            size = length(grain * abs(ratio) * _Fonts_TexelSize.zw) * 0.7071;
            colour = ink.rgb;
            float2 at = (uv - here.xy) / (here.zw - here.xy);
            if (any(at < 0) || any(at > 1)) return 0;   // beside its box the sheet has other glyphs
            float d = tex2Dlod(_Fonts, float4(lerp(there.xy, there.zw, at), 0, 0)).r;
            return saturate((d - 0.5 + ink.a) * max(_GradientScale / max(size, 1e-4), 1.0) + 0.5);
        }

        void surf(Input i, inout SurfaceOutputStandard o)
        {
            float2 uv = i.uv_MainTex;
            float4 c = tex2D(_MainTex, uv) * _Color;
            clip(c.a - _Cutoff);
            float2 grain = fwidth(uv);
            float4 named = tex2Dlod(_Index, float4(uv, 0, 0)) * 255.0;
            int first = (int)(named.x + 0.5) + (int)(named.y + 0.5) * 256;
            int second = (int)(named.z + 0.5) + (int)(named.w + 0.5) * 256;
            float cover = 0, size = length(grain * _Usual.xy) * 0.7071;
            float3 ink = 0;
            if (first > 0)
            {
                cover = glyph_cover(first, uv, grain, ink, size);
                if (second > 0)
                {
                    float3 other;
                    float otherSize, more = glyph_cover(second, uv, grain, other, otherSize);
                    if (more > cover) { cover = more; ink = other; }
                }
            }
            // the field's edge holds while a pixel spans less than about a third of its spread
            float far = saturate((size - 3.5) / 2.5);
            cover *= 1 - far;
            c.rgb = lerp(c.rgb, ink, cover);
            if (far > 0)
            {
                float4 seen = tex2D(_Far, uv) * far;
                c.rgb = c.rgb * (1 - seen.a) + seen.rgb;
                cover += seen.a;
            }
            o.Albedo = c.rgb;
            o.Smoothness = lerp(_Glossiness, _InkGlossiness, cover);   // ink is a surface of its own
            o.Metallic = _InkMetallic * cover;
            o.Alpha = 1;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
