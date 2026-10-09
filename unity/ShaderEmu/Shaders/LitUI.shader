Shader "ShaderEmu/LitUI"
{
    // A panel's plates and buttons as things in the room: the canvas's pictures, lit by the room's
    // baked light volume where they are. (Unity's own UI shader is unlit, and glows in a dim room.)
    Properties
    {
        [PerRendererData] _MainTex ("Sprite", 2D) = "white" {}
        _Color ("Tint", Color) = (1, 1, 1, 1)
        _Light ("Light", Float) = 1
        _Fallback ("Light without a volume (the editor)", Float) = 0.3
        _StencilComp ("Stencil Comparison", Float) = 8
        _Stencil ("Stencil ID", Float) = 0
        _StencilOp ("Stencil Operation", Float) = 0
        _StencilWriteMask ("Stencil Write Mask", Float) = 255
        _StencilReadMask ("Stencil Read Mask", Float) = 255
        _ColorMask ("Color Mask", Float) = 15
    }
    SubShader
    {
        Tags { "Queue"="Transparent" "IgnoreProjector"="True" "RenderType"="Transparent" "PreviewType"="Plane" "CanUseSpriteAtlas"="True" }
        Stencil { Ref [_Stencil] Comp [_StencilComp] Pass [_StencilOp] ReadMask [_StencilReadMask] WriteMask [_StencilWriteMask] }
        Cull Off
        Lighting Off
        ZWrite Off
        ZTest [unity_GUIZTestMode]
        Blend SrcAlpha OneMinusSrcAlpha
        ColorMask [_ColorMask]

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            #include "Packages/red.sim.lightvolumes/Shaders/LightVolumes.cginc"

            sampler2D _MainTex;
            float4 _Color, _TextureSampleAdd;
            float _Light, _Fallback;

            struct v2f {
                float4 pos : SV_Position;
                float2 uv : TEXCOORD0;
                float4 colour : COLOR;
                float3 world : TEXCOORD1;
                float3 normal : TEXCOORD2;
            };

            v2f vert(appdata_full v) {
                v2f o;
                o.pos = UnityObjectToClipPos(v.vertex);
                o.uv = v.texcoord.xy;
                o.colour = v.color * _Color;
                o.world = mul(unity_ObjectToWorld, v.vertex).xyz;
                o.normal = normalize(mul((float3x3)unity_ObjectToWorld, float3(0, 0, -1)));   // a canvas faces -z
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                float4 c = (tex2D(_MainTex, i.uv) + _TextureSampleAdd) * i.colour;
                float3 light = _Fallback;
                if (_UdonLightVolumeEnabled != 0) {
                    float3 L0, L1r, L1g, L1b;
                    LightVolumeSH(i.world, L0, L1r, L1g, L1b);
                    light = max(LightVolumeEvaluate(i.normal, L0, L1r, L1g, L1b), 0.0);
                }
                return float4(c.rgb * light * _Light, c.a);
            }
            ENDCG
        }
    }
}
