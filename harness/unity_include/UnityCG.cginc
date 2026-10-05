// Minimal stand-in for Unity's UnityCG.cginc: only what emulator shaders tend to use.
#ifndef UNITY_CG_INCLUDED
#define UNITY_CG_INCLUDED

#include "HLSLSupport.cginc"
#include "UnityShaderVariables.cginc"

#define UNITY_PI 3.14159265359f
#define UNITY_TWO_PI 6.28318530718f
#define UNITY_HALF_PI 1.57079632679f

struct appdata_base {
    float4 vertex : POSITION;
    float3 normal : NORMAL;
    float4 texcoord : TEXCOORD0;
};

struct appdata_img {
    float4 vertex : POSITION;
    half2 texcoord : TEXCOORD0;
};

struct appdata_full {
    float4 vertex : POSITION;
    float4 tangent : TANGENT;
    float3 normal : NORMAL;
    float4 texcoord : TEXCOORD0;
    float4 texcoord1 : TEXCOORD1;
    float4 texcoord2 : TEXCOORD2;
    float4 texcoord3 : TEXCOORD3;
    fixed4 color : COLOR;
};

inline float4 UnityObjectToClipPos(in float3 pos) {
    return mul(UNITY_MATRIX_VP, mul(unity_ObjectToWorld, float4(pos, 1.0)));
}
inline float4 UnityObjectToClipPos(float4 pos) {
    return UnityObjectToClipPos(pos.xyz);
}

#define TRANSFORM_TEX(tex, name) (tex.xy * name##_ST.xy + name##_ST.zw)

#endif
