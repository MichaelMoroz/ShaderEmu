Shader "ShaderEmu/Beam"
{
    // A hand's beam and its dot: one flat colour, blended by its alpha.
    Properties
    {
        _Color ("Colour", Color) = (0.35, 0.85, 1, 1)
    }
    SubShader
    {
        Tags { "Queue"="Transparent" "RenderType"="Transparent" }
        Blend SrcAlpha OneMinusSrcAlpha
        ZWrite Off

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4 _Color;

            float4 vert(float4 vertex : POSITION) : SV_Position {
                return UnityObjectToClipPos(vertex);
            }

            float4 frag() : SV_Target {
                return _Color;
            }
            ENDCG
        }
    }
}
