// Minimal stand-in for Unity's HLSLSupport.cginc (D3D11 / Shader Model 5 only).
// Unity auto-includes this before CGPROGRAM code; the harness does the same.
#ifndef HLSL_SUPPORT_INCLUDED
#define HLSL_SUPPORT_INCLUDED

#if defined(SHADER_API_D3D11)
#define UNITY_UV_STARTS_AT_TOP 1
#define UNITY_REVERSED_Z 1
#define UNITY_NEAR_CLIP_VALUE (0.0)
#endif

#define UNITY_BRANCH [branch]
#define UNITY_FLATTEN [flatten]
#define UNITY_UNROLL [unroll]
#define UNITY_UNROLLX(_x) [unroll(_x)]
#define UNITY_LOOP [loop]

#define fixed half
#define fixed2 half2
#define fixed3 half3
#define fixed4 half4
#define fixed2x2 half2x2
#define fixed3x3 half3x3
#define fixed4x4 half4x4
#define sampler2D_float sampler2D
#define sampler2D_half sampler2D

#define CBUFFER_START(name) cbuffer name {
#define CBUFFER_END };

#define UNITY_DECLARE_TEX2D(tex) Texture2D tex; SamplerState sampler##tex
#define UNITY_DECLARE_TEX2D_NOSAMPLER(tex) Texture2D tex
#define UNITY_SAMPLE_TEX2D(tex, coord) tex.Sample(sampler##tex, coord)
#define UNITY_INITIALIZE_OUTPUT(type, name) name = (type)0;

#endif
