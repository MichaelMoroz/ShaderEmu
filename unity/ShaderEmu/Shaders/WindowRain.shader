Shader "ShaderEmu/WindowRain"
{
    // The window's pane with rain on its outer face (docs/world/world.md, "The storm").
    //
    // The drops are "Heartfelt" by Martijn Steinrucken aka BigWings (2017), in the version of
    // https://www.shadertoy.com/view/dslczf, ported as it is (below). License: Creative Commons
    // Attribution-NonCommercial-ShareAlike 3.0 Unported -- THIS FILE IS UNDER THAT LICENCE, not the
    // repository's MIT: credit him, no commercial use, and changes shared alike.
    //
    // What is ours: its drops on the pane in metres (a unit of its picture is _Scale m of glass,
    // as tuned in the editor: 0.48), its value as the
    // water's height (_Height mm at 1: a 45 degree cap of that radius is 0.83 mm high), the
    // normal from its differences as it takes them, and the ray traced through that water: bent
    // into the water at the flat glass and out through its surface (Snell, 1.333), the city looked
    // up in the window probe's picture (_Sky: taken outside the pane, far enough to be at
    // infinity), what the surface reflects (Fresnel) mirrored back out (_Rim). Between the drops
    // the pane is clear. (Both panes look along x: across them is world z.)
    Properties
    {
        _Sky ("The city at infinity (the window probe's picture)", CUBE) = "black" {}
        _SkyFlash ("The same as a flash of lightning of 1 lights it", CUBE) = "black" {}
        _Scale ("Metres of glass a unit of Heartfelt's picture", Float) = 0.48
        _Rain ("Heartfelt's rain amount", Range(0,1)) = 0.672
        _Height ("The water's height where Heartfelt's value is 1 (mm)", Float) = 0.83
        _IOR ("Water's index", Float) = 1.333
        _Clear ("The water's own tint", Color) = (0.93, 0.96, 1.0, 1)
        _Rim ("What the inside reflection keeps", Range(0,1)) = 0.738
        [HideInInspector] _Footprint ("A pixel's footprint times this (tests: a supersampled picture as a plain one's)", Float) = 1
        _Debug ("Debug: 1 the sky straight, 2 the slope, 3 cover, 4 the drops' value, 5 the mip, 9 no water", Float) = 0
    }
    SubShader
    {
        Tags { "Queue"="Transparent" "RenderType"="Transparent" "IgnoreProjector"="True" }
        Blend One OneMinusSrcAlpha
        ZWrite Off
        Cull Off

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma target 3.0
            #include "UnityCG.cginc"

            samplerCUBE _Sky, _SkyFlash;
            float4 _Layers; float _Widen, _Footprint, _Scale, _Rain, _Height, _IOR, _Rim, _Debug;
            float4 _Clear;
            float _UdonWeatherFlash;   // lightning (EmuWeather)

            struct v2f {
                float4 pos : SV_Position;
                float3 world : TEXCOORD0;
                float3 normal : TEXCOORD1;
            };

            v2f vert(appdata_base v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.world = mul(unity_ObjectToWorld, v.vertex).xyz;
                o.normal = UnityObjectToWorldNormal(v.normal);
                return o;
            }

            // ---- the drops: a port of "Heartfelt" by Martijn Steinrucken aka BigWings (2017), in the
            // version of https://www.shadertoy.com/view/dslczf (its egg-shaped running drops).
            // License: Creative Commons Attribution-NonCommercial-ShareAlike 3.0 Unported; this file
            // is under that licence, not the repository's MIT. Its functions as there, GLSL to HLSL.

            #define S(a, b, t) smoothstep(a, b, t)

            float3 N13(float p) {   // (from Dave Hoskins)
                float3 p3 = frac(float3(p, p, p) * float3(0.1031, 0.11369, 0.13787));
                p3 += dot(p3, p3.yzx + 19.19);
                return frac(float3((p3.x + p3.y) * p3.z, (p3.x + p3.z) * p3.y, (p3.y + p3.z) * p3.x));
            }
            float N(float t) { return frac(sin(t * 12345.564) * 7658.76); }
            float Saw(float b, float t) { return S(0.0, b, t) * S(1.0, b, t); }
            float3 Rand3(float2 id) { return N13(id.x * 105.45 + id.y * 3543.654); }
            float2 StaticDropCenter(float2 id) { return N13(id.x * 106.45 + id.y * 3543.654).xy - 0.5; }

            // https://iquilezles.org/articles/distfunctions2d/
            float SdEgg(float2 p, float ra, float rb) {
                const float k = 1.7320508;
                p.x = abs(p.x);
                float r = ra - rb;
                return ((p.y < 0.0) ? length(float2(p.x, p.y)) - r :
                        (k * (p.x + r) < p.y) ? length(float2(p.x, p.y - k * r)) :
                                                length(float2(p.x + r, p.y)) - 2.0 * r) - rb;
            }

            // (ours, in the three below: w, a pixel's footprint in units of the picture. A drop, a droplet
            // and a trail are never narrower than it: far away they widen to a pixel, not flicker under one)
            float2 DropLayer2(float2 uv, float t, float w) {
                float2 UV = uv;
                float2 a = float2(6.0, 1.0);
                float2 grid = a * 2.0;
                float2 id = floor(uv * grid);
                float gridFall = N(id.x) / 3.0 + 0.5;
                uv.y += t * gridFall / a.y;
                id = floor(uv * grid);
                float extraDropSpeed = N(id.x);
                uv.y += extraDropSpeed;
                id = floor(uv * grid);
                float2 st = frac(uv * grid) - float2(0.5, 0.0);
                float3 n = N13(id.x * 35.2 + id.y * 2376.1);
                float x = n.x - 0.5;
                float lambda = UV.y * 20.0;
                float wiggle = sin(lambda + sin(lambda));
                x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
                x *= 0.6;
                float ti = frac(t * (gridFall + 0.1) + n.z);
                float slowStart = 0.85;
                float y = (Saw(slowStart, ti) - 0.5) * 0.9 + 0.5;
                float2 p = float2(x, y);
                float dropShape = (ti > slowStart) ? -sin(6.28318 * ti / (1.0 - slowStart)) * 0.5 - 0.5 : 0.0;
                float d = SdEgg((st - p) * a.yx, 0.0, dropShape);
                float diameter = N(id.x + id.y) / 7.0 + 0.2;
                float wc = min(w * grid.x, 0.5);   // the footprint in the cell's units across (d, dd, cd are in those)
                float mainDrop = S(max(diameter / 1.5, wc), 0.0, d);
                float r2 = S(1.0, y, st.y);
                float r = sqrt(r2);
                float cd = abs(st.x - x);
                float thickness = diameter * 0.95;
                float trail = S(max(thickness * r, wc), 0.0, cd);
                float trailFront = S(-0.02, 0.02, st.y - y);
                trail *= r2 * trailFront * 0.5;
                y = UV.y;
                float trail2 = S((thickness - 0.15) * r, 0.0, cd);
                trail2 *= trailFront * n.z;
                float rndX = N(id.x) / 1.5 + 0.5;
                // (ours: his rndY, N(st.y) / 40 + 0.05, hashed st.y: a new value every pixel, a shimmer of
                // white noise in the droplets' size. Left out: its base alone)
                float rndY = 0.05;
                y = frac(y * 11.0 * rndX) + (st.y - 0.5);
                float dd = length(st - float2(x, y));
                float droplets = S(max(trail2 + rndY, wc), 0.0, dd);
                float m = mainDrop + droplets * r * trailFront;
                return float2(m, trail);
            }

            float StaticDrops(float2 uv, float t, float w) {
                uv *= 40.0;
                float2 id = floor(uv);
                float2 center = StaticDropCenter(id) * 0.6;
                float3 rand = Rand3(id);
                uv = frac(uv) - 0.5;
                float d = length(uv - center.xy);
                float drop = S(max(0.3, min(w * 40.0, 0.5)), 0.0, d);
                float fade = Saw(0.10, frac(t + rand.y));
                float intensity = frac(rand.x * 27.0);
                return drop * fade * intensity;
            }

            float2 Drops(float2 uv, float t, float l0, float l1, float l2, float w) {
                float s = StaticDrops(uv, t, w) * l0;
                float2 m1 = DropLayer2(uv, t, w) * l1;
                float2 m2 = DropLayer2(uv * 1.85, t, w * 1.85) * l2;
                float c = s + m1.x + m2.x;
                c = S(0.3, 1.0, c);
                return float2(c, m1.y + m2.y);
            }

            // Heartfelt's mainImage, its rain amount fixed (_Rain): the drops' value at a place of the pane
            float2 HeartfeltAt(float2 uv, float w) {
                float t = _Time.y * 0.2;
                float rainAmount = _Rain;
                float staticDrops = S(-0.5, 1.0, rainAmount) * 2.0;
                float layer1 = S(0.25, 0.75, rainAmount);
                float layer2 = S(0.0, 0.5, rainAmount);
                float3 keep = any(_Layers.xyz) ? _Layers.xyz : 1.0;   // (a test's: which layers)
                return Drops(uv, t, staticDrops * keep.x, layer1 * keep.y, layer2 * keep.z, w);
            }

            #undef S

            float4 _Sky_TexelSize;   // (x: 1 over a face's width)

            // the city in direction d, its mip from how far d moves from one pixel to the next (its screen
            // gradient: a drop's steep side turns the ray a lot a pixel, and takes the city in blurred)
            float3 Sky(float3 d, float part) {   // (part: the share of a pixel this ray stands for, across)
                float k = part * (_Layers.w > 0 ? _Layers.w : 1.0);   // (_Layers.w: a test's scale)
                float3 gx = ddx(d) * k, gy = ddy(d) * k;
                float3 c = texCUBEgrad(_Sky, d, gx, gy).rgb;
                // lightning: the picture as a flash of 1 lights the city, mixed by the flash (what it lights is
                // linear in it). The flash is one value for the whole draw: the branch costs nothing between
                [branch] if (_UdonWeatherFlash > 0.0)
                    c = lerp(c, texCUBEgrad(_SkyFlash, d, gx, gy).rgb, _UdonWeatherFlash);
                return c;
            }

            // the ray out of the water where its surface slopes so (the ray the eye sends, the pane's normal towards the eye)
            float3 Ray(float3 eye, float3 n, float2 slope) {
                float3 water = refract(eye, n, 1.0 / _IOR);
                float3 outward = normalize(-n - slope.x * float3(0, 0, 1) - slope.y * float3(0, 1, 0));
                float3 air = refract(water, -outward, _IOR);
                return normalize(dot(air, air) < 1e-4 ? reflect(water, -outward) : air);
            }

            // what the eye sees through water of this value and slope, premultiplied by where there is water
            // where there is water: from its value alone. Heartfelt's value already rises from 0 smoothly
            // (smoothstep), its slope 0 at the edge, so the ray bends nothing there: the ramp only hides the
            // flat water's own Fresnel reflection where the water starts
            float Wet(float value) { return smoothstep(0.0, 0.05, value); }

            float4 Shade(float3 eye, float3 n, float2 slope, float cover) {
                // through the flat glass into the water (its faces are parallel: the glass itself bends nothing)
                float3 water = refract(eye, n, 1.0 / _IOR);
                // and out through the water's surface, whose normal is the field's gradient: it rises away from the eye
                float3 outward = normalize(-n - slope.x * float3(0, 0, 1) - slope.y * float3(0, 1, 0));
                float3 air = refract(water, -outward, _IOR);
                // what the surface reflects (Fresnel, water to air; all of it past the critical angle)
                // goes back through the glass: the scene mirrored, a little dimmer (_Rim)
                float cosI = saturate(dot(water, outward));
                float sinT = _IOR * sqrt(saturate(1.0 - cosI * cosI));
                float reflected = 1.0;
                if (sinT < 1.0)
                {
                    float cosT = sqrt(1.0 - sinT * sinT);
                    float rs = (_IOR * cosI - cosT) / (_IOR * cosI + cosT), rp = (_IOR * cosT - cosI) / (_IOR * cosT + cosI);
                    reflected = 0.5 * (rs * rs + rp * rp);
                }
                float3 back = reflect(water, -outward);
                float3 mirrored = normalize(back - 2.0 * dot(back, n) * n);   // back out through the pane's plane: towards the city again
                bool leaves = dot(air, air) >= 1e-4;                             // (past the critical angle no ray goes out)
                // (both looked up always: the screen gradients of a ray want every pixel of a block of four to have it)
                float3 through = Sky(normalize(leaves ? air : mirrored), 0.5);
                float3 mirroredSky = Sky(mirrored, 0.5);
                float3 col = ((leaves ? through : 0) * (1.0 - reflected) + mirroredSky * reflected * _Rim) * _Clear.rgb;
                return float4(col * cover, cover);
            }

            float4 frag(v2f i) : SV_Target {
                float3 eye = normalize(i.world - _WorldSpaceCameraPos);   // the ray, from the eye
                float3 n = normalize(i.normal);
                n = dot(n, eye) > 0 ? -n : n;                           // the pane's face towards the eye
                if (_Debug == 9) return 0;                               // (the pane with no water: what a timing compares with)
                float2 uv = i.world.zy / _Scale;                         // Heartfelt's picture on the pane (x across: world z; y up)
                // a pixel's footprint on the pane in the picture's units: its angle from the projection (not a
                // screen derivative), at this distance, stretched by the angle the eye sees the glass at
                float pixel = 2.0 / (unity_CameraProjection._m11 * _ScreenParams.y);
                float wp = length(i.world - _WorldSpaceCameraPos) * pixel / max(abs(dot(eye, n)), 0.2) / _Scale;   // this pixel's
                float w = wp * _Footprint * (_Widen > 0 ? _Widen : 1.0);   // what the drops widen to (a test's supersampled picture keeps the plain one's)
                // The water over the pixel: its value on a 3 x 3 lattice across the pixel's footprint (h apart,
                // never under 1/2160 of a unit: Heartfelt's half pixel at 1080p), and from each quarter's four
                // corners its slope and its cover, each quarter shaded on its own and the four averaged. One ray
                // a pixel aliased where a drop is steep (its rim, its outline): the rays there turn faster than
                // a pixel. No screen derivatives in any of this (over 2 x 2 blocks of pixels they drew a grid)
                float hs = max(0.5 * wp, 1.0 / 2160.0);
                float v[3][3];
                [unroll] for (int a = 0; a < 3; a++)
                    [unroll] for (int b = 0; b < 3; b++)
                        v[a][b] = HeartfeltAt(uv + float2(a - 1, b - 1) * hs, w).x;
                float toSlope = _Height / (2.0 * hs * _Scale * 1000.0);   // value difference over 2 hs: dh/dz, dh/dy, mm over mm
                if (_Debug == 1) return float4(Sky(eye, 1.0), 1);
                if (_Debug == 2) return float4(saturate(abs(float2(v[2][1] - v[0][1], v[1][2] - v[1][0]) * toSlope)), 0, 1);
                if (_Debug == 3) return float4(Wet(v[1][1]).xxx, 1);
                if (_Debug == 4) return float4(v[1][1], 0, 0, 1);
                if (_Debug == 5)   // the mip the ray straight through the middle takes (a quarter's: half a pixel), 6 white
                {
                    float3 d = Ray(eye, n, float2(v[2][1] - v[0][1], v[1][2] - v[1][0]) * toSlope);
                    float turn = 0.5 * max(length(ddx(d)), length(ddy(d))) / (1.5707963 * _Sky_TexelSize.x);
                    return float4(saturate(log2(max(turn, 1.0)) / 6.0).xxx, 1);
                }
                float4 sum = 0;
                [unroll] for (int qa = 0; qa < 2; qa++)
                    [unroll] for (int qb = 0; qb < 2; qb++)
                    {
                        float c00 = v[qa][qb], c10 = v[qa + 1][qb], c01 = v[qa][qb + 1], c11 = v[qa + 1][qb + 1];
                        // (corners hs apart, each difference taken twice: their sum over 2 hs)
                        float2 slope = float2(c10 + c11 - c00 - c01, c01 + c11 - c00 - c10) * toSlope;
                        // where there is water, the share of its corners that are wet (not the corners' mean
                        // value's: one wet corner of four made the whole quarter wet, and far away the glass
                        // came out 1.8 times as bright as supersampled)
                        float cover = 0.25 * (Wet(c00) + Wet(c10) + Wet(c01) + Wet(c11));
                        sum += Shade(eye, n, slope, cover);
                    }
                return sum * 0.25;
            }
            ENDCG
        }
    }
}
