Shader "ShaderEmu/LitText"
{
    // TextMesh Pro's lettering as print on a thing in the room: the mobile distance field shader's
    // face, lit by the room's baked light volume where it is. (TextMesh Pro's own is unlit.)
    Properties
    {
        _FaceColor ("Face Color", Color) = (1, 1, 1, 1)
        _FaceDilate ("Face Dilate", Range(-1, 1)) = 0
        _OutlineWidth ("Outline Thickness (unused: TextMesh Pro reads it)", Range(0, 1)) = 0
        _OutlineSoftness ("Outline Softness (unused)", Range(0, 1)) = 0
        _WeightNormal ("Weight Normal", float) = 0
        _WeightBold ("Weight Bold", float) = .5
        _ShaderFlags ("Flags", float) = 0
        _ScaleRatioA ("Scale RatioA", float) = 1
        _ScaleRatioB ("Scale RatioB", float) = 1
        _ScaleRatioC ("Scale RatioC", float) = 1
        _MainTex ("Font Atlas", 2D) = "white" {}
        _TextureWidth ("Texture Width", float) = 512
        _TextureHeight ("Texture Height", float) = 512
        _GradientScale ("Gradient Scale", float) = 5
        _ScaleX ("Scale X", float) = 1
        _ScaleY ("Scale Y", float) = 1
        _PerspectiveFilter ("Perspective Correction", Range(0, 1)) = 0.875
        _Sharpness ("Sharpness", Range(-1, 1)) = 0
        _VertexOffsetX ("Vertex OffsetX", float) = 0
        _VertexOffsetY ("Vertex OffsetY", float) = 0
        _ClipRect ("Clip Rect", vector) = (-32767, -32767, 32767, 32767)
        _Light ("Light", Float) = 1
        _Fallback ("Light without a volume", Float) = 0.3
        _StencilComp ("Stencil Comparison", Float) = 8
        _Stencil ("Stencil ID", Float) = 0
        _StencilOp ("Stencil Operation", Float) = 0
        _StencilWriteMask ("Stencil Write Mask", Float) = 255
        _StencilReadMask ("Stencil Read Mask", Float) = 255
        _CullMode ("Cull Mode", Float) = 0
        _ColorMask ("Color Mask", Float) = 15
    }
    SubShader
    {
        Tags { "Queue"="Transparent" "IgnoreProjector"="True" "RenderType"="Transparent" }
        Stencil { Ref [_Stencil] Comp [_StencilComp] Pass [_StencilOp] ReadMask [_StencilReadMask] WriteMask [_StencilWriteMask] }
        Cull [_CullMode]
        ZWrite Off
        Lighting Off
        ZTest [unity_GUIZTestMode]
        Blend One OneMinusSrcAlpha
        ColorMask [_ColorMask]

        Pass
        {
            CGPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            #include "Packages/red.sim.lightvolumes/Shaders/LightVolumes.cginc"

            sampler2D _MainTex;
            float4 _FaceColor;
            float _FaceDilate, _WeightNormal, _WeightBold, _ScaleRatioA, _GradientScale, _ScaleX, _ScaleY;
            float _PerspectiveFilter, _Sharpness, _VertexOffsetX, _VertexOffsetY, _Light, _Fallback;

            struct appdata {
                float4 vertex : POSITION;
                float3 normal : NORMAL;
                float4 color : COLOR;
                float2 texcoord0 : TEXCOORD0;
                float2 texcoord1 : TEXCOORD1;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct v2f {
                float4 pos : SV_Position;
                float4 colour : COLOR;
                float2 uv : TEXCOORD0;
                float2 edge : TEXCOORD1;      // the distance field's scale and bias
                float3 world : TEXCOORD2;
                float3 normal : TEXCOORD3;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            v2f vert(appdata v) {
                v2f o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(v2f, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                float4 at = v.vertex;
                at.xy += float2(_VertexOffsetX, _VertexOffsetY);
                o.pos = UnityObjectToClipPos(at);
                float2 pixel = o.pos.w;
                pixel /= float2(_ScaleX, _ScaleY) * abs(mul((float2x2)UNITY_MATRIX_P, _ScreenParams.xy));
                float scale = rsqrt(dot(pixel, pixel)) * abs(v.texcoord1.y) * _GradientScale * (_Sharpness + 1);
                float3 normal = normalize(mul((float3x3)unity_ObjectToWorld, float3(0, 0, -1)));   // a canvas faces -z
                if (UNITY_MATRIX_P[3][3] == 0)
                    scale = lerp(abs(scale) * (1 - _PerspectiveFilter), scale, abs(dot(normal, normalize(WorldSpaceViewDir(at)))));
                float weight = lerp(_WeightNormal, _WeightBold, step(v.texcoord1.y, 0)) / 4.0;
                weight = (weight + _FaceDilate) * _ScaleRatioA * 0.5;
                o.edge = float2(scale, (0.5 - weight) * scale - 0.5);
                o.colour = v.color * _FaceColor;
                o.uv = v.texcoord0;
                o.world = mul(unity_ObjectToWorld, at).xyz;
                o.normal = normal;
                return o;
            }

            float4 frag(v2f i) : SV_Target {
                float cover = saturate(tex2D(_MainTex, i.uv).a * i.edge.x - i.edge.y) * i.colour.a;
                float3 light = _Fallback;
                if (_UdonLightVolumeEnabled != 0) {
                    float3 L0, L1r, L1g, L1b;
                    LightVolumeSH(i.world, L0, L1r, L1g, L1b);
                    light = max(LightVolumeEvaluate(normalize(i.normal), L0, L1r, L1g, L1b), 0.0);
                }
                return float4(i.colour.rgb * light * _Light * cover, cover);
            }
            ENDCG
        }
    }
}
