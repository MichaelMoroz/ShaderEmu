Shader "ShaderEmu/gpu"
{
    // The machine's GPU (docs/gpu.md). GPUDraw is a mesh drawn into the GPU's own colour and
    // depth target, its vertex shader reading the guest's command list from the state texture.
    // GPUControl is an update zone on the state texture: list drawn, input delivered.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _GpuTarget ("The GPU's own colour target", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        Lighting Off
        Blend One Zero

        Pass
        {
            Name "GPUDraw"
            ZTest LEqual
            ZWrite On

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag

            Texture2D<uint4> _State;
            #define GPU_STATE _State
            #include "src/gpu.h"

            // In Unity the mesh carries its vertex number; here SV_VertexID is the same thing.
            gpu_varyings vert(uint id : SV_VertexID) {
                return gpu_vertex(id);
            }
            float4 frag(gpu_varyings i) : SV_Target {
                return gpu_fragment(i);
            }
            ENDCG
        }

        Pass
        {
            Name "GPUControl"
            ZTest Off

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex CustomRenderTextureVertexShader
            #pragma fragment frag

            #include "crt.cginc"
            #include "UnityCG.cginc"
            // The host's input for this frame: pointer position over the display panel and the
            // panel's size, both in window pixels; buttons; up to four key events (Linux key
            // code, bit 31 while pressed) numbered from _InputKeySeq.
            uniform float4 _InputPointer;
            uniform uint _InputButtons, _InputKeySeq, _InputKeyCount;
            uniform uint _InputKey0, _InputKey1, _InputKey2, _InputKey3;
            #define GPU_STATE _SelfTexture2D
            #define GPU_INPUT
            #include "src/gpu.h"

            uint4 frag(v2f_customrendertexture i) : SV_Target {
                uint2 dim;
                _SelfTexture2D.GetDimensions(dim.x, dim.y);
                return gpu_control(i.globalTexcoord.xy * dim);
            }
            ENDCG
        }
    }
}
