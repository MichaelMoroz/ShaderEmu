Shader "ShaderEmu/GvmCommitBlit"
{
    // The commit and the operations' first write as blits, no camera (docs/REGRESSIONS.md of GPU-VM, step 2 item 3): a
    // blit's two triangles are two patches, a quad domain at factor 8 is 81 points each, and a geometry shader of 32
    // instances gives 5,184 calls a blit. Passes 0 to 3 are GvmCommit's four, a word of the texel each, 16 of the
    // commit's points a call (82,944 >= _CommitPoints); pass 4 is GvmOps' write, two of its instances (a core's slot,
    // two quads) a call. The commit's points never meet (one entry a word of a texel in a core, no two cores a texel).
    Properties
    {
        _S0 ("State 0", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend Off
        CGINCLUDE
        #pragma target 5.0
        #pragma vertex amp_vert
        #pragma hull amp_hull
        #pragma domain amp_dom
        #pragma geometry amp_geo
        #pragma fragment amp_frag
        static const uint AMP_F = 8, AMP_N = AMP_F + 1, AMP_INST = 32;
        struct amp_v { float4 p : INTERNALTESSPOS; };
        struct amp_tf { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; };
        struct amp_d { float2 uv : TEXCOORD0; nointerpolation uint patch : TEXCOORD1; };   // (patch: the domain shader's SV_PrimitiveID)
        amp_v amp_vert(float4 v : POSITION) { amp_v o; o.p = v; return o; }
        amp_tf amp_hc(InputPatch<amp_v, 3> I) {
            amp_tf o;
            o.e[0] = o.e[1] = o.e[2] = o.e[3] = AMP_F;
            o.i[0] = o.i[1] = AMP_F;
            return o;
        }
        [domain("quad")]
        [partitioning("integer")]
        [outputtopology("point")]
        [patchconstantfunc("amp_hc")]
        [outputcontrolpoints(3)]
        amp_v amp_hull(InputPatch<amp_v, 3> I, uint id : SV_OutputControlPointID) { return I[id]; }
        [domain("quad")]
        amp_d amp_dom(amp_tf h, const OutputPatch<amp_v, 3> I, float2 uv : SV_DomainLocation, uint pid : SV_PrimitiveID) { amp_d o; o.uv = uv; o.patch = pid; return o; }
        uint amp_call(amp_d d, uint pid, uint inst) {   // this call's number, 0 to 5,183
            uint x = (uint)round(d.uv.x * AMP_F), y = (uint)round(d.uv.y * AMP_F);
            return ((pid * AMP_N + y) * AMP_N + x) * AMP_INST + inst;
        }
        ENDCG
        Pass
        {
            ColorMask R
            CGPROGRAM
            #define _Comp 0u
            #include "commit_blit.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask G
            CGPROGRAM
            #define _Comp 1u
            #include "commit_blit.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask B
            CGPROGRAM
            #define _Comp 2u
            #include "commit_blit.cginc"
            ENDCG
        }
        Pass
        {
            ColorMask A
            CGPROGRAM
            #define _Comp 3u
            #include "commit_blit.cginc"
            ENDCG
        }
        Pass   // 4: the write, GvmOps' GVM_WRITE: a slot's piece as two quads of RAM, from the stage
        {
            CGPROGRAM
            #define _Comp 0u
            #include "tick.cginc"
            Texture2D<uint4> _Plan;
            o2f write_corner(uint vid, uint inst) {
                uint c = inst / OP_SLOTS, s = inst % OP_SLOTS;
                uint4 j0 = _Plan.Load(int3(2 * s, c, 0));
                bool on = c < _NCores && _Plan.Load(int3(2 * s + 1, c, 0)).z != 0;
                uint t0 = j0.x >> 4, t1 = (j0.y - 1) >> 4, x0 = t0 % RAM_W, y0 = t0 / RAM_W, xe = x0 + t1 - t0 + 1;
                bool second = vid >= 6;
                o2f r;
                r.vertex = corner(vid, second ? 0 : (float)x0, (float)(second ? xe - RAM_W : min(xe, RAM_W)), (float)(y0 + (second ? 1 : 0)),
                                  (float)(y0 + (second ? 2 : 1)), (float)RAM_W, (float)_RamRows, on && (!second || xe > RAM_W));
                r.j0 = uint4(t0, t1, inst, 0);
                r.j1 = 0;
                return r;
            }
            [maxvertexcount(16)]
            [instance(32)]
            void amp_geo(point amp_d I[1], inout TriangleStream<o2f> s, uint inst : SV_GSInstanceID) {
                uint pid = I[0].patch;
                uint call = amp_call(I[0], pid, inst);
                [unroll] for (uint k = 0; k < 2; k++) {
                    uint n = 2 * call + k;
                    [branch] if (n < _NCores * OP_SLOTS) {
                        // a quad's six vertices are corners 0 1 2 / 3 4 5: as a strip 0 1 2 4, the second quad 6 7 8 10
                        [unroll] for (uint q = 0; q < 2; q++) {
                            o2f a = write_corner(6 * q, n);
                            [branch] if (a.vertex.x > -1.5) {
                                s.Append(a);
                                s.Append(write_corner(6 * q + 1, n));
                                s.Append(write_corner(6 * q + 2, n));
                                s.Append(write_corner(6 * q + 4, n));
                                s.RestartStrip();
                            }
                        }
                    }
                }
            }
            uint4 amp_frag(o2f i) : SV_Target { return op_write_frag(i); }
            ENDCG
        }
    }
}
