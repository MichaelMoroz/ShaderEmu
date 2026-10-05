// Minimal stand-in for Unity's UnityShaderVariables.cginc. Unity puts these in named
// cbuffers; here they live in $Globals so the harness can set them by name.
#ifndef UNITY_SHADER_VARIABLES_INCLUDED
#define UNITY_SHADER_VARIABLES_INCLUDED

float4 _Time;          // (t/20, t, t*2, t*3)
float4 _SinTime;       // sin(t/8), sin(t/4), sin(t/2), sin(t)
float4 _CosTime;
float4 unity_DeltaTime;
float3 _WorldSpaceCameraPos;
float4 _ProjectionParams;
float4 _ScreenParams;
float4 _ZBufferParams;
float4 unity_OrthoParams;  // w = 1 for orthographic cameras

float4x4 unity_ObjectToWorld;
float4x4 unity_WorldToObject;
float4x4 unity_MatrixV;
float4x4 unity_MatrixVP;
float4x4 glstate_matrix_projection;

#define UNITY_MATRIX_M unity_ObjectToWorld
#define UNITY_MATRIX_V unity_MatrixV
#define UNITY_MATRIX_P glstate_matrix_projection
#define UNITY_MATRIX_VP unity_MatrixVP
#define UNITY_MATRIX_MVP mul(unity_MatrixVP, unity_ObjectToWorld)

#endif
