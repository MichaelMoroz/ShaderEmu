Shader "ShaderEmu/GvmOpsBlit"
{
    // A round of the operations as blits where the harness draws meshes (a camera's render costs the world 0.16 ms):
    // pass 0 each core's and slot's piece into _Plan (the stage's and the write's vertex work, once), pass 1 the
    // stage as a gather over its texture, pass 2 the progress, a texel a core, pass 3 how many cores have more queued.
    Properties
    {
        _Ram ("RAM", 2D) = "black" {}
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
        #include "UnityCG.cginc"
        #include "tick.cginc"
        Texture2D<uint4> _Plan;
        struct b2f { float4 vertex : SV_POSITION; };
        b2f blit_vert(float4 v : POSITION) { b2f o; o.vertex = UnityObjectToClipPos(v); return o; }
        ENDCG
        Pass   // 0: the plan, two texels a slot: (d0, d1, s0, kind) and (the source turned, the blit's lag, on, 0)
        {
            CGPROGRAM
            #pragma vertex blit_vert
            #pragma fragment blit_frag
            uint4 blit_frag(b2f i) : SV_Target {
                uint2 px = uint2(i.vertex.xy);
                uint inst = px.y * OP_SLOTS + px.x / 2;
                opd o; uint d0, d1, s0; bool on;
                slot_piece(inst, o, d0, d1, s0, on);
                uint sh = (o.dst & 3) * 8, dl = d0 - s0;
                [branch] if ((px.x & 1) == 0) return uint4(d0, d1, s0, o.kind);
                return uint4(sh != 0 ? (o.src << sh) | (o.src >> (32 - sh)) : o.src, o.kind == GVM_OP_BLIT && dl != 0 && dl < d1 - d0 ? dl : 0,
                             on ? 1u : 0u, 0);
            }
            ENDCG
        }
        Pass   // 1: the stage, a row a core's slot: what op_stage_vert's quad covers, by op_stage_frag
        {
            CGPROGRAM
            #pragma vertex blit_vert
            #pragma fragment blit_frag
            uint4 blit_frag(b2f i) : SV_Target {
                uint2 px = uint2(i.vertex.xy);
                uint c = px.y / OP_SLOTS, s = px.y % OP_SLOTS;
                [branch] if (c >= _NCores) return 0;
                uint4 j1 = _Plan.Load(int3(2 * s + 1, c, 0));
                [branch] if (j1.z == 0) return 0;
                uint4 j0 = _Plan.Load(int3(2 * s, c, 0));
                [branch] if (px.x >= ((j0.y - 1) >> 4) - (j0.x >> 4) + 1) return 0;
                o2f f;
                f.vertex = i.vertex;
                f.j0 = j0;
                f.j1 = uint4(j1.x, j1.y, 0, 0);
                return op_stage_frag(f);
            }
            ENDCG
        }
        Pass   // 2: the progress after the round, a texel a core (op_prog_vert's point)
        {
            CGPROGRAM
            #pragma vertex blit_vert
            #pragma fragment blit_frag
            uint4 blit_frag(b2f i) : SV_Target {
                uint c = (uint)i.vertex.x;
                plan_t P = plan(c, NONE);
                return uint4(state_any(22, c).z, P.d, P.p, P.rf);
            }
            ENDCG
        }
        Pass   // 3: the cores with operations still queued after the pass's rounds, into one texel (read back: a second round)
        {
            CGPROGRAM
            #pragma vertex blit_vert
            #pragma fragment blit_frag
            uint4 blit_frag(b2f i) : SV_Target {
                uint n = 0;
                [loop] for (uint c = 0; c < _NCores; c++) {
                    uint4 qs = state_any(22, c), os = _OpState.Load(int3(c, 0, 0));
                    n += (os.x != qs.z ? qs.x > 0 : os.y < qs.x) ? 1u : 0u;
                }
                return uint4(n, 0, 0, 0);
            }
            ENDCG
        }
    }
}
