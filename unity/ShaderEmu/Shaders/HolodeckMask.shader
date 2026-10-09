Shader "ShaderEmu/HolodeckMask"
{
    // The holodeck's own faces (docs/holodeck.md). Where they are in view, and while a program
    // has a frame to show, it marks the stencil and empties colour and depth, so that
    // GpuHolodeck.shader's world shows through them however far it reaches. Without a frame it
    // draws nothing, and the room is its grid.
    Properties
    {
        _Back ("Colour where the scene has nothing", Color) = (0.01, 0.012, 0.02, 1)
        _State ("Machine state texture", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "Queue"="Transparent-499" "RenderType"="Opaque" }   // after everything solid and the sky

        CGINCLUDE
        #pragma target 5.0
        #include "UnityCG.cginc"
        float4 _Back;
        Texture2D<uint4> _State;
        #define GPU_STATE _State
        #define GPU_VOLUME
        #include "src/gpu.cginc"

        struct v2f {
            float4 pos : SV_Position;
            UNITY_VERTEX_OUTPUT_STEREO
        };

        v2f vert(appdata_base v) {
            v2f o;
            UNITY_SETUP_INSTANCE_ID(v);
            UNITY_INITIALIZE_OUTPUT(v2f, o);
            UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
            uint4 frame = ram(VOLUME_LIST);
            // no frame: off the screen
            o.pos = frame.r != 0 && frame.g != 0 ? UnityObjectToClipPos(v.vertex) : float4(0, 0, -2, 1);
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
