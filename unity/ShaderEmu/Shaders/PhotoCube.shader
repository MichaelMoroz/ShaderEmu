Shader "ShaderEmu/PhotoCube"
{
    // The cube before the screen that VRChat's camera is put into for a photo of a file (docs/apps/export.md, "A photo";
    // Photo.shader draws the file): a box of glowing edges and a faint pulsing fill, seen by everyone while the guest
    // offers a photo. Not drawn into the camera's own picture while it is inside (that one is the file's).
    Properties
    {
        _Color ("Colour", Color) = (0.3, 0.85, 1, 1)
    }
    SubShader
    {
        Tags { "Queue" = "Transparent" "RenderType" = "Transparent" "IgnoreProjector" = "True" }
        Cull Off
        ZWrite Off
        Blend SrcAlpha OneMinusSrcAlpha

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"

            float4 _Color;
            float _VRChatCameraMode;

            struct v2f { float4 pos : SV_Position; float3 local : TEXCOORD0; };

            v2f vert(float4 vertex : POSITION)
            {
                v2f o;
                float3 cam = mul(unity_WorldToObject, float4(_WorldSpaceCameraPos, 1)).xyz;
                bool taking = _VRChatCameraMode > 0.5 && all(abs(cam) <= 0.5);
                o.pos = taking ? float4(2, 2, 2, 1) : UnityObjectToClipPos(vertex);
                o.local = vertex.xyz;
                return o;
            }

            float4 frag(v2f i) : SV_Target
            {
                // how near an edge of the cube (two coordinates near +-0.5)
                float3 a = abs(i.local) * 2;
                float second = a.x + a.y + a.z - max(a.x, max(a.y, a.z)) - min(a.x, min(a.y, a.z));
                float edge = smoothstep(0.94, 0.99, second);
                float pulse = 0.5 + 0.5 * sin(_Time.y * 3.5);
                return float4(_Color.rgb, lerp(0.05 + 0.10 * pulse, 0.55 + 0.4 * pulse, edge));
            }

            ENDCG
        }
    }
}
