Shader "ShaderEmu/WindowCity"
{
    // A window onto a city at night that is not there: for every pixel of the pane the view ray
    // is followed through a grid of blocks, one building each, so the towers shift against
    // each other as the viewer moves (and each eye sees its own view in a headset).
    Properties
    {
        _Height ("Height of the window above the streets, in metres", Float) = 42
        _Block ("Side of a city block", Float) = 16
        _Glow ("Brightness", Float) = 1
        _Glass ("How much of the room the glass reflects", Range(0, 1)) = 0.5
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" }

        Pass
        {
            Tags { "LightMode"="ForwardBase" }   // for the room's reflection probe
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float _Height, _Block, _Glow, _Glass;

            struct v2f {
                float4 pos : SV_Position;
                float3 world : TEXCOORD0;
            };

            v2f vert(appdata_base v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.world = mul(unity_ObjectToWorld, v.vertex).xyz;
                return o;
            }

            float hash(float2 p) {
                p = frac(p * float2(123.34, 456.21));
                p += dot(p, p + 45.32);
                return frac(p.x * p.y);
            }

            float3 sky(float3 d) {
                float3 c = lerp(float3(0.10, 0.13, 0.26), float3(0.004, 0.006, 0.02), saturate(d.y * 1.6 + 0.1));
                c += float3(0.30, 0.16, 0.10) * pow(saturate(1.0 - abs(d.y) * 6.0), 3.0);   // the city's glow on the haze
                // stars: one per cell of the direction, most cells empty
                float2 at = float2(atan2(d.x, d.z), asin(clamp(d.y, -1, 1))) * 90.0;
                float2 cell = floor(at);
                float star = hash(cell);
                if (star > 0.93 && d.y > 0.03) {
                    float2 centre = cell + 0.5 + (float2(hash(cell + 7.1), hash(cell + 3.7)) - 0.5) * 0.6;
                    c += saturate(1.0 - length(at - centre) * 4.0) * (star - 0.93) * 30.0;
                }
                float3 moon = normalize(float3(-0.35, 0.42, 0.84));
                float m = dot(d, moon);
                if (m > 0.9985) c = float3(0.95, 0.93, 0.85);
                else c += float3(0.25, 0.25, 0.3) * pow(saturate(m), 60.0);
                return c;
            }

            float4 frag(v2f i) : SV_Target {
                // the pane's own frame: across, up, and out through the glass
                float3 right = normalize(unity_ObjectToWorld._m00_m10_m20);
                float3 up = normalize(unity_ObjectToWorld._m01_m11_m21);
                float3 outward = normalize(unity_ObjectToWorld._m02_m12_m22);
                float3 rel = i.world - unity_ObjectToWorld._m03_m13_m23;
                float3 view = normalize(i.world - _WorldSpaceCameraPos);
                float3 d = float3(dot(view, right), dot(view, up), dot(view, outward));
                d.z = max(d.z, 0.02);
                float3 o = float3(dot(rel, right) + 8.0 * _Block + 5.0, dot(rel, up) + _Height, 0.0);

                float3 c = sky(d);
                float reach = d.y < 0 ? -o.y / d.y : 1e6;   // where the ray meets the street
                float found = reach;
                float3 shade = 0;
                bool hit = false;

                float2 cell = floor(o.xz / _Block);
                float2 dir = float2(d.x >= 0 ? 1 : -1, 1);
                float2 safe = float2(abs(d.x) < 1e-4 ? 1e-4 : d.x, d.z);
                float2 next = (cell + max(dir, 0)) * _Block;
                float2 tMax = (next - o.xz) / safe, tDelta = _Block / abs(safe);
                [loop]
                for (int k = 0; k < 56 && !hit; k++) {
                    float seed = hash(cell);
                    // nothing in the first two rows of blocks, and some lots stay empty
                    if (cell.y >= 2 && seed > 0.22) {
                        float halfSide = _Block * (0.26 + 0.14 * hash(cell + 11.3));
                        float tall = 14.0 + 80.0 * seed * seed + 30.0 * hash(cell + 2.9) * step(0.8, seed);
                        float3 lo = float3((cell.x + 0.5) * _Block - halfSide, 0, (cell.y + 0.5) * _Block - halfSide);
                        float3 hi = float3(lo.x + 2.0 * halfSide, tall, lo.z + 2.0 * halfSide);
                        float3 inv = 1.0 / float3(safe.x, abs(d.y) < 1e-4 ? 1e-4 : d.y, safe.y);
                        float3 t0 = (lo - o) * inv, t1 = (hi - o) * inv;
                        float3 tNear = min(t0, t1), tFar = max(t0, t1);
                        float enter = max(max(tNear.x, tNear.y), tNear.z), leave = min(min(tFar.x, tFar.y), tFar.z);
                        if (enter < leave && enter > 0 && enter < found) {
                            hit = true;
                            found = enter;
                            float3 p = o + d * enter;
                            if (tNear.y >= tNear.x && tNear.y >= tNear.z) {
                                shade = float3(0.012, 0.013, 0.018);   // a roof
                            } else {
                                // a wall: storeys of windows, some of them shade
                                float along = tNear.x > tNear.z ? p.z : p.x;
                                float2 pane = float2(along / 2.4, p.y / 3.3);
                                float2 which = floor(pane), within = frac(pane);
                                float on = hash(which + cell * 17.0 + (tNear.x > tNear.z ? 31.0 : 0.0));
                                bool glass = within.x > 0.18 && within.x < 0.82 && within.y > 0.25 && within.y < 0.80 && p.y < tall - 1.5;
                                float3 warm = lerp(float3(1.0, 0.72, 0.38), float3(0.65, 0.85, 1.0), step(0.75, hash(which + 5.5)));
                                shade = float3(0.016, 0.018, 0.026) * (tNear.x > tNear.z ? 1.0 : 0.6);
                                if (glass) shade = on > 0.62 ? warm * (0.5 + 0.9 * hash(which + 1.3)) : float3(0.02, 0.025, 0.04);
                                if (p.y > tall - 0.8 && seed > 0.7 && frac(_Time.y * 0.5 + seed * 7.0) < 0.5) shade += float3(0.9, 0.05, 0.05) * step(0.9, within.x);
                            }
                        }
                    }
                    if (min(tMax.x, tMax.y) > found) break;
                    if (tMax.x < tMax.y) { tMax.x += tDelta.x; cell.x += dir.x; }
                    else { tMax.y += tDelta.y; cell.y += 1; }
                }
                if (hit) {
                    c = shade;
                } else if (reach < 1e5) {
                    // the street: dark, with lamps along the block edges
                    float3 p = o + d * reach;
                    float2 g = abs(frac(p.xz / _Block) - 0.5);
                    float road = step(0.42, max(g.x, g.y));
                    float2 lamp = abs(frac(p.xz / (_Block * 0.25)) - 0.5);
                    c = float3(0.012, 0.012, 0.016) + road * float3(0.05, 0.04, 0.03)
                        + road * float3(1.0, 0.7, 0.35) * saturate(1.0 - length(lamp) * 9.0) * 0.8;
                }
                // haze swallows what is far away
                float3 haze = float3(0.12, 0.09, 0.10);
                c = lerp(c, haze, hit || reach < 1e5 ? saturate(1.0 - exp(-found / 900.0)) : 0.0);
                c *= _Glow;
                // the glass itself: the room mirrored in it, more at a slant, and a faint sheen
                float3 inward = -normalize(unity_ObjectToWorld._m02_m12_m22);
                float3 mirrored = reflect(view, inward);
                float3 room = DecodeHDR(UNITY_SAMPLE_TEXCUBE_LOD(unity_SpecCube0, mirrored, 1.5), unity_SpecCube0_HDR);
                float slant = pow(1.0 - saturate(dot(-view, inward)), 4.0);
                c = c * 0.93 + room * _Glass * (0.10 + 0.75 * slant) + float3(0.010, 0.014, 0.018);
                return float4(c, 1);
            }
            ENDCG
        }
    }
}
