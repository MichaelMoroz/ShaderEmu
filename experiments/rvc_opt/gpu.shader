Shader "ShaderEmu/gpu"
{
    // The machine's GPU (docs/gpu.md): real triangles, rasterised by the graphics card.
    //   GPUDraw     is drawn as a mesh of GPU_TRIANGLES triangles into the GPU's own colour and
    //               depth target (in a world: a camera on a private layer). The vertex shader
    //               reads the guest's command list and vertex buffers from the state texture
    //               and places each vertex; the fragment shader textures and colours.
    //   GPUControl  is one more update zone on the state texture: it marks a submitted list as
    //               drawn.
    // Depth test, blending and culling are fixed. The guest chooses, per draw, how vertices are
    // projected and how fragments are coloured, from a small set of modes.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
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
            #define GPU_STATE _SelfTexture2D
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
