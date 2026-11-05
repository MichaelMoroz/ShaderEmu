Shader "ShaderEmu/Plate"
{
    // A panel's plate, or a keyboard's caps: a lit surface with its lettering printed on it.
    // _Index names the two glyphs nearest a place (0: none), _Glyphs is three texels a glyph (its
    // box here, its box in _Fonts, its colour and weight), _Fonts the fonts' distance fields.
    // The lettering is the fields at every distance: where a pixel holds several of their texels
    // it is asked at up to sixteen places of the pixel and the answers are averaged.
    Properties
    {
        _MainTex ("Surface", 2D) = "white" {}
        _Color ("Tint", Color) = (1, 1, 1, 1)
        [NoScaleOffset] _Index ("Glyphs of each place", 2D) = "black" {}
        [NoScaleOffset] _Glyphs ("Glyphs", 2D) = "black" {}
        [NoScaleOffset] _Fonts ("Fonts' distance fields", 2D) = "black" {}
        _Usual ("The sheet's texels across the plate, for its usual lettering", Vector) = (0, 0, 0, 0)
        _Canvas ("Where the lettering's canvas lies in the surface's picture (start, size)", Vector) = (0, 0, 1, 1)
        _GradientScale ("The fields' gradient scale", Float) = 9
        _Glossiness ("Smoothness", Range(0, 1)) = 0.3
        _InkGlossiness ("The lettering's smoothness", Range(0, 1)) = 0.65
        _InkMetallic ("The lettering's metal", Range(0, 1)) = 0
        _InkGlow ("The lettering's own light", Range(0, 2)) = 0
        _Flat ("Shown as drawn, not lit", Range(0, 1)) = 0
        _Bold ("The lettering's strokes thickened (of the field's range)", Range(0, 0.2)) = 0
        _Cutoff ("Cut out below this alpha", Range(0, 1)) = 0
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }
        CGPROGRAM
        // a printed plate mirrors nothing: no reflection probe is read (a flat tone of the room's light stands in)
        #define _GLOSSYREFLECTIONS_OFF 1
        #pragma surface surf Standard fullforwardshadows vertex:vert addshadow
        #pragma target 4.0

        sampler2D _MainTex, _Fonts, _Index, _Glyphs;   // the last two are point filtered
        float4 _Color, _Glyphs_TexelSize, _Fonts_TexelSize, _Usual, _Canvas;
        float _Glossiness, _InkGlossiness, _InkMetallic, _InkGlow, _Flat, _Bold, _Cutoff, _GradientScale;

        struct Input { float2 uv_MainTex; };

        // A key a hand holds (EmuKeyboard.Sink): what of a cap lies in its box sinks; its feet stay.
        float4 _UdonPress0Centre, _UdonPress0X, _UdonPress0Y, _UdonPress0Up, _UdonPress1Centre, _UdonPress1X, _UdonPress1Y, _UdonPress1Up;
        float3 pressed(float3 w, float4 c, float4 x, float4 y, float4 n)
        {
            float3 d = w - c.xyz;
            float below = -dot(d, n.xyz);
            bool inside = abs(dot(d, x.xyz)) < x.w && abs(dot(d, y.xyz)) < y.w && below > -0.003 && below < n.w;
            return c.w > 0 && inside ? w - n.xyz * c.w : w;
        }

        void vert(inout appdata_full v)
        {
            if (_UdonPress0Centre.w <= 0 && _UdonPress1Centre.w <= 0) return;
            float3 w = mul(unity_ObjectToWorld, v.vertex).xyz;
            w = pressed(pressed(w, _UdonPress0Centre, _UdonPress0X, _UdonPress0Y, _UdonPress0Up), _UdonPress1Centre, _UdonPress1X, _UdonPress1Y, _UdonPress1Up);
            v.vertex.xyz = mul(unity_WorldToObject, float4(w, 1)).xyz;
        }

        float4 glyph_word(int n) { return tex2Dlod(_Glyphs, float4((float2(n & 1023, n >> 10) + 0.5) * _Glyphs_TexelSize.xy, 0, 0)); }

        // How much of a pixel at uv a glyph's ink covers; grain is the uv a pixel spans.
        // size: the fields' texels a pixel spans for this glyph.
        float glyph_cover(int glyph, float2 uv, float2 grain, out float3 colour, out float size)
        {
            int n = (glyph - 1) * 3;
            float4 here = glyph_word(n), there = glyph_word(n + 1), ink = glyph_word(n + 2);
            float2 ratio = (there.zw - there.xy) / (here.zw - here.xy);
            // the fields' texels a pixel spans, as the mean of its two ways: by the longer way alone a plate
            // seen aslant had every edge softened to grey, and its lettering paled along the line
            float2 span = grain * abs(ratio) * _Fonts_TexelSize.zw;
            size = sqrt(max(span.x * span.y, 1e-12));
            colour = ink.rgb;
            float2 at = (uv - here.xy) / (here.zw - here.xy);
            if (any(at < 0) || any(at > 1)) return 0;   // beside its box the sheet has other glyphs
            float d = tex2Dlod(_Fonts, float4(lerp(there.xy, there.zw, at), 0, 0)).r;
            return saturate((d - 0.5 + ink.a + _Bold + 0.05 * saturate(size / 4)) * max(_GradientScale / max(size, 1e-4), 1.0) + 0.5);
        }

        // The lettering at uv, of a place that spans `grain`: its cover, and its ink times that.
        float4 lettering(float2 uv, float2 grain)
        {
            float4 named = tex2Dlod(_Index, float4(uv, 0, 0)) * 255.0;
            int first = (int)(named.x + 0.5) + (int)(named.y + 0.5) * 256;
            int second = (int)(named.z + 0.5) + (int)(named.w + 0.5) * 256;
            float cover = 0, size;
            float3 ink = 0;
            if (first > 0)
            {
                cover = glyph_cover(first, uv, grain, ink, size);
                if (second > 0)
                {
                    float3 other;
                    float more = glyph_cover(second, uv, grain, other, size);
                    if (more > cover) { cover = more; ink = other; }
                }
            }
            return float4(ink * cover, cover);
        }

        void surf(Input i, inout SurfaceOutputStandard o)
        {
            float2 uv = i.uv_MainTex;
            float4 c = tex2D(_MainTex, uv) * _Color;
            clip(c.a - _Cutoff);
            uv = (uv - _Canvas.xy) / _Canvas.zw;   // (a keyboard's picture has its caps' sides under the canvas: world/pc_faces.py)
            float2 across = ddx(uv), down = ddy(uv), grain = abs(across) + abs(down);
            // the fields' texels a pixel spans, for the plate's usual lettering: a field's edge is
            // good for one or two of them, so beyond that the pixel is cut into n by n places
            float both = sqrt(max(grain.x * _Usual.x * grain.y * _Usual.y, 1e-12));
            int n = both < 1.5 ? 1 : both < 3.5 ? 2 : both < 7.0 ? 3 : 4;
            float4 sum = 0;
            for (int a = 0; a < 4; a++)
                for (int b = 0; b < 4; b++)
                {
                    if (a >= n || b >= n) continue;
                    float2 at = (float2(a, b) + 0.5) / n - 0.5;
                    sum += lettering(uv + across * at.x + down * at.y, grain / n);
                }
            sum /= n * n;
            if (any(uv < 0) || any(uv > 1)) sum = 0;   // (no lettering off the canvas)
            float cover = sum.a;
            float3 ink = cover > 1e-4 ? sum.rgb / cover : 0;
            c.rgb = lerp(c.rgb, ink, cover);
            float3 glow = sum.rgb;
            // a panel is something to read: most of it is shown as it is drawn, and a little of it takes the room's light
            o.Albedo = c.rgb * (1 - _Flat);
            o.Emission = c.rgb * _Flat + glow * _InkGlow;
            o.Smoothness = lerp(_Glossiness, _InkGlossiness, cover);   // ink is a surface of its own
            o.Metallic = _InkMetallic * cover;
            o.Alpha = 1;
        }
        ENDCG
    }
    FallBack "Diffuse"
}
