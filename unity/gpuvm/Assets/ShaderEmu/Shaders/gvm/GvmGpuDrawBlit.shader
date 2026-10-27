Shader "ShaderEmu/GvmGpuDrawBlit"
{
    // GvmGpuDraw as blits, no camera (docs/REGRESSIONS.md of GPU-VM, step 2 item 4): the same 8 passes, each a blit into
    // the GPU's target with its depth buffer. A blit's two triangles are two patches, a quad domain at factor 8 is 81
    // points each, and a geometry shader of 32 instances gives 5,184 calls, 13 of the device's triangles a call.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        Lighting Off
        CGINCLUDE
        #pragma target 5.0
        #pragma vertex amp_vert
        #pragma hull amp_hull
        #pragma domain amp_dom
        #pragma geometry amp_geo
        #pragma fragment frag
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
        #define GPU_BLIT
        ENDCG
        Pass
        {
            Name "GPU0"
            Blend One Zero
            ZTest LEqual
            ZWrite On
            CGPROGRAM
            #define _GpuPass 0
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU1"
            Blend SrcAlpha OneMinusSrcAlpha
            ZTest LEqual
            ZWrite Off
            CGPROGRAM
            #define _GpuPass 1
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU2"
            Blend SrcAlpha One
            ZTest LEqual
            ZWrite Off
            CGPROGRAM
            #define _GpuPass 2
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU3"
            Blend DstColor Zero, Zero One
            ZTest LEqual
            ZWrite Off
            CGPROGRAM
            #define _GpuPass 3
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU4"
            Blend One Zero
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #define _GpuPass 4
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU5"
            Blend SrcAlpha OneMinusSrcAlpha
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #define _GpuPass 5
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU6"
            Blend SrcAlpha One
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #define _GpuPass 6
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass
        {
            Name "GPU7"
            Blend DstColor Zero, Zero One
            ZTest Always
            ZWrite Off
            CGPROGRAM
            #define _GpuPass 7
            #include "GvmGpuDrawPass.cginc"
            ENDCG
        }
        Pass   // 8: the depth cleared to far, as the camera's clear (Depth) did, before passes 0 to 7
        {
            Name "CLEAR"
            ColorMask 0
            ZTest Always
            ZWrite On
            CGPROGRAM
            struct clr { float4 position : SV_Position; };
            [maxvertexcount(3)]
            [instance(32)]
            void amp_geo(point amp_d I[1], inout TriangleStream<clr> s, uint inst : SV_GSInstanceID) {
                uint pid = I[0].patch;
                if (amp_call(I[0], pid, inst) != 0) return;
#if defined(UNITY_REVERSED_Z)
                float z = 0.0;
#else
                float z = 1.0;
#endif
                clr o;
                o.position = float4(-1.0, -1.0, z, 1.0); s.Append(o);
                o.position = float4(3.0, -1.0, z, 1.0); s.Append(o);
                o.position = float4(-1.0, 3.0, z, 1.0); s.Append(o);
            }
            float4 frag(clr i) : SV_Target { return 0; }
            ENDCG
        }
    }
}
