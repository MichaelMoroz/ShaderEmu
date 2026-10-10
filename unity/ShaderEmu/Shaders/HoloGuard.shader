Shader "ShaderEmu/HoloGuard"
{
    // A holodeck's own things (the seat, the stand, its panel, its screen) are before the
    // program's world however near that reaches (docs/holodeck.md): a copy of each draws
    // nothing but a mark in the stencil where the thing itself is seen, after the room's mask
    // and before the program's world, and no pass of that world draws on a pixel so marked.
    SubShader
    {
        Tags { "Queue"="Transparent-498" "RenderType"="Opaque" }
        Pass
        {
            ColorMask 0
            ZWrite Off
            ZTest LEqual
            Offset -1, -1      // the copy lies on the thing itself
            Cull Off
            Stencil { Ref 215 Comp Always Pass Replace }   // 87 + 128 (GpuHolodeck.shader)

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
