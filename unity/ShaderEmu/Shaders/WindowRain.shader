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
    // as tuned in the editor: 0.84), its value as the
    // water's height (_Height mm at 1, as tuned in the editor: 0.83),
    // its exact gradient (HeartfeltD), and the ray traced through that water: bent into the water at
    // the flat glass and out through its surface (Snell, 1.333), the city looked up in the window
    // probe's picture (_Sky: taken outside the pane, far enough to be at infinity), what the surface
    // reflects (Fresnel) back into the room, looked up in the room's picture (_Room, times _Rim).
    // Between the drops the pane is clear. (Both panes look along x: across them is world z.)
    Properties
    {
        _Sky ("The city at infinity (the window probe's picture)", CUBE) = "black" {}
        _SkyFlash ("The same as a flash of lightning of 1 lights it", CUBE) = "black" {}
        _Room ("The room as the pane sees it (what the water's surface reflects back in)", CUBE) = "black" {}
        _Mist ("Mist on the dry glass: the city behind it blurred (Heartfelt's fog; drops and trails clear it)", Range(0, 1)) = 0.15
        _MistBlur ("The mist's blur, degrees", Float) = 4.47
        _Scale ("Metres of glass a unit of Heartfelt's picture", Float) = 0.84
        _Rain ("Heartfelt's rain amount", Range(0,1)) = 1
        _Height ("The water's height where Heartfelt's value is 1 (mm)", Float) = 0.83
        _IOR ("Water's index", Float) = 1.333
        _Clear ("The water's own tint", Color) = (0.93, 0.96, 1.0, 1)
        _Rim ("What the inside reflection keeps", Range(0,1)) = 1
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

            samplerCUBE _Sky, _SkyFlash, _Room;
            float4 _Layers; float _Mist, _MistBlur, _Test, _Widen, _Footprint, _Scale, _Rain, _Height, _IOR, _Rim, _Debug;
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

            // ---- the same with its exact gradient (ours): each value as (value, d/du, d/dv) of the place in
            // Heartfelt's picture, carried through the same steps. A floor, a frac's cell or a hash is constant
            // in its piece, so its gradient is 0 there; only what is drawn varies. The value is the one above.

            // smoothstep(a, b, t) and its gradient, a, b and t each with theirs
            float3 SD(float a, float2 da, float b, float2 db, float t, float2 dt) {
                float den = b - a;
                float q = (t - a) / den;
                float qc = saturate(q);
                float2 dq = (dt - da) / den - (t - a) * (db - da) / (den * den);
                return float3(qc * qc * (3.0 - 2.0 * qc), (q > 0.0 && q < 1.0 ? 6.0 * qc * (1.0 - qc) : 0.0) * dq);
            }

            // SdEgg's gradient by its argument (as SdEgg: ra 0)
            float2 SdEggGrad(float2 p, float rb) {
                const float k = 1.7320508;
                float sx = p.x < 0.0 ? -1.0 : 1.0;
                p.x = abs(p.x);
                float r = -rb;
                float2 g = (p.y < 0.0) ? p :
                           (k * (p.x + r) < p.y) ? float2(p.x, p.y - k * r) : float2(p.x + r, p.y);
                g /= max(length(g), 1e-6);
                return float2(g.x * sx, g.y);
            }

            float4 DropLayer2D(float2 uv, float t, float w) {
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
                float2 dstx = float2(grid.x, 0.0), dsty = float2(0.0, grid.y);   // st.x's and st.y's gradients
                float3 n = N13(id.x * 35.2 + id.y * 2376.1);
                float x = n.x - 0.5;
                float lambda = UV.y * 20.0;
                float wiggle = sin(lambda + sin(lambda));
                float2 dwiggle = float2(0.0, cos(lambda + sin(lambda)) * (1.0 + cos(lambda)) * 20.0);
                float2 dx = dwiggle * (0.5 - abs(x)) * (n.z - 0.5) * 0.6;
                x += wiggle * (0.5 - abs(x)) * (n.z - 0.5);
                x *= 0.6;
                float ti = frac(t * (gridFall + 0.1) + n.z);
                float slowStart = 0.85;
                float y = (Saw(slowStart, ti) - 0.5) * 0.9 + 0.5;
                float2 p = float2(x, y);
                float dropShape = (ti > slowStart) ? -sin(6.28318 * ti / (1.0 - slowStart)) * 0.5 - 0.5 : 0.0;
                float2 q = (st - p) * a.yx;
                float d = SdEgg(q, 0.0, dropShape);
                float2 gq = SdEggGrad(q, dropShape);
                float2 dd_ = gq.x * (dstx - dx) * a.y + gq.y * dsty * a.x;   // (p.y is the time's: no gradient)
                float diameter = N(id.x + id.y) / 7.0 + 0.2;
                float wc = min(w * grid.x, 0.5);
                float3 mainDrop = SD(max(diameter / 1.5, wc), 0, 0.0, 0, d, dd_);
                float3 r2 = SD(1.0, 0, y, 0, st.y, dsty);
                float r = sqrt(r2.x);
                float2 dr = r > 1e-5 ? r2.yz / (2.0 * r) : 0;
                float sgn = st.x - x < 0.0 ? -1.0 : 1.0;
                float cd = abs(st.x - x);
                float2 dcd = sgn * (dstx - dx);
                float thickness = diameter * 0.95;
                float3 trailFront = SD(-0.02, 0, 0.02, 0, st.y - y, dsty);
                float yUV = UV.y;
                float3 trail2 = SD((thickness - 0.15) * r, (thickness - 0.15) * dr, 0.0, 0, cd, dcd);
                trail2 = float3(trail2.x * trailFront.x * n.z, (trail2.yz * trailFront.x + trail2.x * trailFront.yz) * n.z);
                float rndX = N(id.x) / 1.5 + 0.5;
                float rndY = 0.05;
                float yd = frac(yUV * 11.0 * rndX) + (st.y - 0.5);
                float2 dyd = float2(0.0, 11.0 * rndX) + dsty;
                float2 rel = st - float2(x, yd);
                float dd = length(rel);
                float2 ddd = (rel.x * (dstx - dx) + rel.y * (dsty - dyd)) / max(dd, 1e-6);
                bool wide = trail2.x + rndY > wc;
                float3 droplets = SD(wide ? trail2.x + rndY : wc, wide ? trail2.yz : 0, 0.0, 0, dd, ddd);
                // m = mainDrop + droplets r trailFront
                float prod = droplets.x * r * trailFront.x;
                float2 dprod = droplets.yz * r * trailFront.x + droplets.x * dr * trailFront.x + droplets.x * r * trailFront.yz;
                float trail = S(max(thickness * r, wc), 0.0, cd) * r2.x * trailFront.x * 0.5;   // (its value only: the mist's)
                return float4(mainDrop.x + prod, mainDrop.yz + dprod, trail);
            }

            float3 StaticDropsD(float2 uv, float t, float w) {
                uv *= 40.0;
                float2 id = floor(uv);
                float2 center = StaticDropCenter(id) * 0.6;
                float3 rand = Rand3(id);
                uv = frac(uv) - 0.5;
                float2 rel = uv - center.xy;
                float d = length(rel);
                float2 dd = rel / max(d, 1e-6) * 40.0;
                float3 drop = SD(max(0.3, min(w * 40.0, 0.5)), 0, 0.0, 0, d, dd);
                float fade = Saw(0.10, frac(t + rand.y));
                float intensity = frac(rand.x * 27.0);
                return drop * fade * intensity;
            }

            // HeartfeltAt's value (x) and its gradient by the place in the picture (yz)
            float4 HeartfeltD(float2 uv, float w) {   // (w: Heartfelt's trail value, as HeartfeltAt's y)
                float t = _Time.y * 0.2;
                float rainAmount = _Rain;
                float staticDrops = S(-0.5, 1.0, rainAmount) * 2.0;
                float layer1 = S(0.25, 0.75, rainAmount);
                float layer2 = S(0.0, 0.5, rainAmount);
                float3 keep = any(_Layers.xyz) ? _Layers.xyz : 1.0;
                float3 s0 = StaticDropsD(uv, t, w) * staticDrops * keep.x;
                float4 m1 = DropLayer2D(uv, t, w) * layer1 * keep.y;
                float4 m2 = DropLayer2D(uv * 1.85, t, w * 1.85) * layer2 * keep.z;
                m2.yz *= 1.85;
                float3 c = s0 + m1.xyz + m2.xyz;
                return float4(SD(0.3, 0, 1.0, 0, c.x, c.yz), m1.w + m2.w);
            }

            #undef S

            float4 _Sky_TexelSize, _Room_TexelSize;   // (x: 1 over a face's width: the city's 1024, the room's 256)

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

            // The optics of water sloping so: the ray that leaves it (or, past the critical angle, none), the
            // ray its surface reflects back out through the pane, and the share it reflects (Fresnel, water to air)
            struct Optics { float3 air; float3 mirrored; float reflected; float leaves; };

            // the share water sloping so reflects (Fresnel, water to air; all of it past the critical angle),
            // and the share that leaves it (x, y)
            float2 Shares(float3 eye, float3 n, float2 slope) {
                float3 water = refract(eye, n, 1.0 / _IOR);
                float3 outward = normalize(-n - slope.x * float3(0, 0, 1) - slope.y * float3(0, 1, 0));
                float cosI = saturate(dot(water, outward));
                float sinT = _IOR * sqrt(saturate(1.0 - cosI * cosI));
                if (sinT >= 1.0) return float2(1.0, 0.0);
                float cosT = sqrt(1.0 - sinT * sinT);
                float rs = (_IOR * cosI - cosT) / (_IOR * cosI + cosT), rp = (_IOR * cosT - cosI) / (_IOR * cosT + cosI);
                float r = 0.5 * (rs * rs + rp * rp);
                return float2(r, 1.0 - r);
            }

            Optics Trace(float3 eye, float3 n, float2 slope) {
                Optics o;
                // through the flat glass into the water (its faces are parallel: the glass itself bends nothing)
                float3 water = refract(eye, n, 1.0 / _IOR);
                // and out through the water's surface, whose normal is the field's gradient: it rises away from the eye
                float3 outward = normalize(-n - slope.x * float3(0, 0, 1) - slope.y * float3(0, 1, 0));
                float3 air = refract(water, -outward, _IOR);
                float cosI = saturate(dot(water, outward));
                float sinT = _IOR * sqrt(saturate(1.0 - cosI * cosI));
                o.reflected = 1.0;
                if (sinT < 1.0)
                {
                    float cosT = sqrt(1.0 - sinT * sinT);
                    float rs = (_IOR * cosI - cosT) / (_IOR * cosI + cosT), rp = (_IOR * cosT - cosI) / (_IOR * cosT + cosI);
                    o.reflected = 0.5 * (rs * rs + rp * rp);
                }
                float3 back = reflect(water, -outward);
                // what the surface reflects goes back through the glass into the room, and shows the room: its
                // lamps on a drop's rim against the dark city are what makes a drop seen at night (before, it was
                // turned back out to the city, and a drop blended into what was behind it)
                o.mirrored = normalize(back);
                o.leaves = dot(air, air) >= 1e-4 ? 1.0 : 0.0;
                o.air = o.leaves > 0 ? normalize(air) : o.mirrored;      // (a direction always: a lookup wants one)
                return o;
            }

            // the room in direction d over a cone of `spread` radians, as SkyCone
            float3 RoomCone(float3 d, float spread) {
                float lod = log2(max(spread * (_Layers.w > 0 ? _Layers.w : 1.0) / (1.5707963 * _Room_TexelSize.x), 1.0));
                return texCUBElod(_Room, float4(d, lod)).rgb;
            }

            // the city in direction d over a cone of `spread` radians: the mip whose texel spans it
            float3 SkyCone(float3 d, float spread) {
                float lod = log2(max(spread * (_Layers.w > 0 ? _Layers.w : 1.0) / (1.5707963 * _Sky_TexelSize.x), 1.0));
                float3 c = texCUBElod(_Sky, float4(d, lod)).rgb;
                [branch] if (_UdonWeatherFlash > 0.0)   // lightning, as Sky
                    c = lerp(c, texCUBElod(_SkyFlash, float4(d, lod)).rgb, _UdonWeatherFlash);
                return c;
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
                float hs = 0.5 * wp;
                float v[3][3];
                float2 g[3][3];   // and the exact slope there (HeartfeltD's gradient, mm over mm)
                float tr[3][3];   // and Heartfelt's trail value (the mist's)
                float toPoint = _Height / (_Scale * 1000.0);
                [unroll] for (int a = 0; a < 3; a++)
                    [unroll] for (int b = 0; b < 3; b++)
                    {
                        float4 h = HeartfeltD(uv + float2(a - 1, b - 1) * hs, w);
                        tr[a][b] = h.w;
                        v[a][b] = h.x;
                        g[a][b] = h.yz * toPoint;
                    }
                float toSlope = _Height / (2.0 * hs * _Scale * 1000.0);   // value difference over 2 hs: dh/dz, dh/dy, mm over mm
                if (_Debug == 1) return float4(Sky(eye, 1.0), 1);
                if (_Debug == 2) return float4(saturate(abs(float2(v[2][1] - v[0][1], v[1][2] - v[1][0]) * toSlope)), 0, 1);
                if (_Debug == 3) return float4(Wet(v[1][1]).xxx, 1);
                if (_Debug == 4) return float4(v[1][1], 0, 0, 1);
                if (_Debug == 6)   // (a check: the exact gradient against differences of 1e-4, at the middle)
                {
                    float3 g = HeartfeltD(uv, w);
                    float ee = 1e-4;
                    float2 num = float2(HeartfeltAt(uv + float2(ee, 0), w).x - HeartfeltAt(uv - float2(ee, 0), w).x,
                                        HeartfeltAt(uv + float2(0, ee), w).x - HeartfeltAt(uv - float2(0, ee), w).x) / (2.0 * ee);
                    return float4(g.yz, num);
                }
                if (_Debug == 5)   // the mip the ray straight through the middle takes (a quarter's: half a pixel), 6 white
                {
                    float3 d = Ray(eye, n, float2(v[2][1] - v[0][1], v[1][2] - v[1][0]) * toSlope);
                    float turn = 0.5 * max(length(ddx(d)), length(ddy(d))) / (1.5707963 * _Sky_TexelSize.x);
                    return float4(saturate(log2(max(turn, 1.0)) / 6.0).xxx, 1);
                }
                Optics q[4];
                float cover[4], throughShare[4];
                float2 slope[4];
                [unroll] for (int qa = 0; qa < 2; qa++)
                    [unroll] for (int qb = 0; qb < 2; qb++)
                    {
                        float c00 = v[qa][qb], c10 = v[qa + 1][qb], c01 = v[qa][qb + 1], c11 = v[qa + 1][qb + 1];
                        int k = qa * 2 + qb;
                        // (corners hs apart, each difference taken twice: their sum over 2 hs)
                        slope[k] = float2(c10 + c11 - c00 - c01, c01 + c11 - c00 - c10) * toSlope;
                        q[k] = Trace(eye, n, slope[k]);
                        // where there is water, the share of its corners that are wet (not the corners' mean
                        // value's: one wet corner of four made the whole quarter wet, and far away the glass
                        // came out 1.8 times as bright as supersampled)
                        cover[k] = 0.25 * (Wet(c00) + Wet(c10) + Wet(c01) + Wet(c11));
                    }
                // What a quarter reflects and lets through, and where to: over the exact slopes at its four
                // corners, not at its mean slope. A drop's rim, steep enough to reflect it all, is a fifth of its
                // radius: from a few metres thinner than a quarter, and the mean slope over a quarter flattened
                // it (the reflection a fifth of the supersampled picture's). Each quarter's two rays are its
                // corners' rays weighted by what each reflects or lets through, their lookups blurred over how
                // far those corners' rays spread (below): still one lookup a ray a quarter
                Optics pt[3][3];
                [unroll] for (int a2 = 0; a2 < 3; a2++)
                    [unroll] for (int b2 = 0; b2 < 3; b2++)
                        pt[a2][b2] = Trace(eye, n, g[a2][b2]);
                float spreadAir[4], spreadMirror[4];
                [unroll] for (int k = 0; k < 4; k++)
                {
                    int qa = k >> 1, qb = k & 1;
                    spreadAir[k] = 0;
                    spreadMirror[k] = 0;
                    if (_Test == 4)   // (a test's: at the mean slope, as before)
                    {
                        throughShare[k] = q[k].leaves * (1.0 - q[k].reflected);
                        continue;
                    }
                    float3 air = 0, mirrored = 0;
                    float r = 0, t = 0;
                    [unroll] for (int j = 0; j < 4; j++)
                    {
                        Optics c = pt[qa + (j >> 1)][qb + (j & 1)];
                        float tj = c.leaves * (1.0 - c.reflected);
                        air += c.air * tj;
                        mirrored += c.mirrored * c.reflected;
                        r += c.reflected;
                        t += tj;
                    }
                    q[k].air = t > 1e-4 ? normalize(air) : q[k].air;
                    q[k].mirrored = r > 1e-4 ? normalize(mirrored) : q[k].mirrored;
                    q[k].reflected = 0.25 * r;
                    throughShare[k] = 0.25 * t;
                    [unroll] for (int j2 = 0; j2 < 4; j2++)   // how far the corners' rays that count lie from the quarter's
                    {
                        Optics c = pt[qa + (j2 >> 1)][qb + (j2 & 1)];
                        float tj = c.leaves * (1.0 - c.reflected);
                        spreadAir[k] = max(spreadAir[k], tj > 0.05 * t ? length(c.air - q[k].air) : 0.0);
                        spreadMirror[k] = max(spreadMirror[k], c.reflected > 0.05 * r ? length(c.mirrored - q[k].mirrored) : 0.0);
                    }
                }
                // Each quarter's lookups over the cone its rays sweep: how far they turn to the next quarter's
                // across and up, half a pixel away (inside the drop's lens a ray turns a long way in a pixel, and
                // a 2 x 2 block's screen gradient straddles drops), and never under the quarter's own angle
                float3 sum = 0;
                float wet = 0;
                float own = 0.5 * pixel;   // (half the pixel's angle, from the projection)
                [unroll] for (int k = 0; k < 4; k++)
                {
                    int across = k ^ 2, up = k ^ 1;
                    float sweepAir = max(length(q[k].air - q[across].air), length(q[k].air - q[up].air));
                    float sweepMirror = max(length(q[k].mirrored - q[across].mirrored), length(q[k].mirrored - q[up].mirrored));
                    float3 through, mirroredSky;
                    if (_Test == 1)   // (a test's: the screen gradient's mip, as before)
                    {
                        through = Sky(q[k].air, 0.5);
                        mirroredSky = RoomCone(q[k].mirrored, 0.0);
                    }
                    else
                    {
                        through = SkyCone(q[k].air, max(max(sweepAir, spreadAir[k]), own));
                        mirroredSky = RoomCone(q[k].mirrored, max(max(sweepMirror, spreadMirror[k]), own));
                    }
                    float3 col = (through * throughShare[k] + mirroredSky * q[k].reflected * _Rim) * _Clear.rgb;
                    sum += col * cover[k];
                    wet += cover[k];
                }
                // Mist on the dry glass (Heartfelt's fog: his picture is blurred everywhere but where a drop or a
                // trail has cleared it): the city behind seen blurred, so a drop, clear, stands out of it. A
                // trail at its fullest (his trail value's peak, 0.5) clears it all
                [branch] if (_Mist > 0.0)
                {
                    float3 misty = SkyCone(eye, radians(_MistBlur));
                    float mist = 0;
                    [unroll] for (int k = 0; k < 4; k++)
                    {
                        int qa = k >> 1, qb = k & 1;
                        float trail = 0.25 * (tr[qa][qb] + tr[qa + 1][qb] + tr[qa][qb + 1] + tr[qa + 1][qb + 1]);
                        mist += _Mist * (1.0 - saturate(trail / 0.5)) * (1.0 - cover[k]);
                    }
                    sum += misty * _Clear.rgb * mist;
                    wet += mist;
                }
                return float4(sum, wet) * 0.25;
            }
            ENDCG
        }
    }
}
