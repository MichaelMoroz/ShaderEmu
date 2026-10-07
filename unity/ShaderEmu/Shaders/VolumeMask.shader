Shader "ShaderEmu/VolumeMask"
{
    // The volume display's box (docs/volume.md). Where its faces are in view it marks the
    // stencil and empties colour and depth, so that GpuVolume.shader's scene shows through it
    // however far that reaches behind the box.
    Properties
    {
        _Back ("Colour where the scene has nothing", Color) = (0.01, 0.012, 0.02, 1)
    }
    SubShader
    {
        Tags { "Queue"="Transparent-499" "RenderType"="Opaque" }   // after everything solid and the sky

        CGINCLUDE
        #include "UnityCG.cginc"
        float4 _Back;

        struct v2f {
            float4 pos : SV_Position;
            UNITY_VERTEX_OUTPUT_STEREO
        };

        v2f vert(appdata_base v) {
            v2f o;
            UNITY_SETUP_INSTANCE_ID(v);
            UNITY_INITIALIZE_OUTPUT(v2f, o);
            UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
            o.pos = UnityObjectToClipPos(v.vertex);
            return o;
        }
        ENDCG

        Pass
        {
            // the faces not hidden by the room: stencil only
            Cull Off
            ZTest LEqual
            ZWrite Off
            ColorMask 0
            Stencil { Ref 87 Comp Always Pass Replace }

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            float4 frag(v2f i) : SV_Target { return 0; }
            ENDCG
        }

        Pass
        {
            // there: the background, and depth as far as it goes
            Cull Off
            ZTest Always
            ZWrite On
            Stencil { Ref 87 Comp Equal }

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            float4 frag(v2f i, out float depth : SV_Depth) : SV_Target {
#if defined(UNITY_REVERSED_Z)
                depth = 0;
#else
                depth = 1;
#endif
                return _Back;
            }
            ENDCG
        }
    }
}
