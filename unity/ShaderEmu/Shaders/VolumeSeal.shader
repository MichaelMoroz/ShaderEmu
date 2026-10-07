Shader "ShaderEmu/VolumeSeal"
{
    // Drawn on the volume display's box after its scene: gives the marked pixels the depth of
    // the box's own faces and unmarks them, so what is drawn later sits in front of or behind
    // the box, not somewhere in the scene inside it.
    SubShader
    {
        Tags { "Queue"="Transparent-497" "RenderType"="Opaque" }

        Pass
        {
            Cull Back
            ZTest Always
            ZWrite On
            ColorMask 0
            Stencil { Ref 87 Comp Equal Pass Zero }

            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

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

            float4 frag(v2f i) : SV_Target { return 0; }
            ENDCG
        }
    }
}
