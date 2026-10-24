Shader "ShaderEmu/GvmOps"
{
    // A round of the tick's operations is three draws (gpu_vm/tick/STATE.md); _GvmDraw says which this material makes,
    // a keyword each: the stage (into the stage texture), the write (into RAM), the progress (into an _OpState).
    // A mesh's vertex v is the harness's instance v / 6 (12), vertex v % 6 (12); the harness's viewports are mapped
    // into the whole targets (the stage 256 * OP_SLOTS rows, an _OpState 256 texels).
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
        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma multi_compile GVM_STAGE GVM_WRITE GVM_PROG
            #pragma vertex gvm_op_vert
            #pragma fragment pfrag
            #include "tick.cginc"
        #if defined(GVM_STAGE)
            o2f gvm_op_vert(uint id : SV_VertexID) {
                o2f r = op_stage_vert(id % 6, id / 6);
                if (id / 6 >= _NCores * OP_SLOTS) r.vertex = float4(-2, -2, 0, 1);
                if (r.vertex.x > -1.5) r.vertex.y = 1 - (1 - r.vertex.y) * (float)_NCores / 256.0;
                return r;
            }
            uint4 pfrag(o2f i) : SV_Target { return op_stage_frag(i); }
        #elif defined(GVM_WRITE)
            Texture2D<uint4> _Plan;   // GvmOpsBlit's pass 0: the piece op_write_vert would plan, made once a round
            o2f gvm_op_vert(uint id : SV_VertexID) {
                uint vid = id % 12, inst = id / 12, c = inst / OP_SLOTS, s = inst % OP_SLOTS;
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
            uint4 pfrag(o2f i) : SV_Target { return op_write_frag(i); }
        #else
            p2f gvm_op_vert(uint id : SV_VertexID) {
                p2f o = op_prog_vert(id);
                o.vertex = id < _NCores ? float4((float)(2 * id + 1) / 256.0 - 1, 0, 0, 1) : float4(-2, -2, 0, 1);
                return o;
            }
            uint4 pfrag(p2f i) : SV_Target { return op_prog_frag(i); }
        #endif
            ENDCG
        }
    }
}
