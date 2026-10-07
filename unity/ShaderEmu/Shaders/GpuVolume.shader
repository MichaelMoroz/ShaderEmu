Shader "ShaderEmu/GpuVolume"
{
    // The volume display (docs/volume.md): a flat screen that is a window onto the guest's 3D
    // frame. The screen is a plane of the guest camera's view, the near plane unless _Plane
    // moves it out, with the picture's rectangle there fitted to it; the scene is drawn about
    // it as the visitor's own eyes see it, wherever VolumeMask.shader has marked the stencil.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
        _ScreenSize ("Width and height of the screen", Vector) = (1.28, 0.8, 0, 0)
        _Plane ("Where the screen is between the near plane (0) and the far plane (1)", Range(0, 1)) = 0
    }
    SubShader
    {
        Tags { "Queue"="Transparent-498" "RenderType"="Opaque" }   // after the mask, and after the sky

        Pass
        {
            Cull Off      // the guest's own culling is not known
            ZTest LEqual
            ZWrite On
            Stencil { Ref 87 Comp Equal }

            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom
            #pragma fragment frag
            #pragma multi_compile_instancing
            #include "UnityCG.cginc"

            Texture2D<uint4> _State;
            float4 _ScreenSize;
            float _Plane;
            #define GPU_STATE _State
            #define GPU_VOLUME
            #include "src/gpu.cginc"

            struct mesh_point {
                uint id : SV_VertexID;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };
            struct volume_point {
                uint triangle_id : TEXCOORD0;
                UNITY_VERTEX_OUTPUT_STEREO
            };
            struct volume_out {
                gpu_varyings v;
                UNITY_VERTEX_OUTPUT_STEREO
            };

            volume_point vert(mesh_point v) {
                volume_point o;
                UNITY_SETUP_INSTANCE_ID(v);
                UNITY_INITIALIZE_OUTPUT(volume_point, o);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(o);
                o.triangle_id = v.id;
                return o;
            }

            [maxvertexcount(3)]
            void geom(point volume_point i[1], inout TriangleStream<volume_out> stream) {
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(i[0]);
                uint first = i[0].triangle_id * 3, base;
                uint4 head;
                if (!volume_command(first, base, head)) return;
                for (uint k = 0; k < 3; k++) {
                    float3 eye;
                    float2 planes, focal;
                    volume_out o;
                    UNITY_INITIALIZE_OUTPUT(volume_out, o);
                    o.v = volume_vertex(first + k, base, head, eye, planes, focal);
                    // the screen's distance from the guest's camera: from near to far in equal ratios
                    float near = max(planes.x, 1e-4), at = near * pow(max(planes.y, near) / near, _Plane);
                    // metres a guest unit: the picture's rectangle at that distance, 2 at / focal
                    // across, fits the screen
                    float scale = min(_ScreenSize.x * focal.x, _ScreenSize.y * focal.y) / (2.0 * at);
                    // the camera stands that far before the screen, on the visitor's side; what
                    // is nearer to it than the screen comes out of the wall
                    float3 local = float3(eye.x, eye.y, -eye.z - at) * scale;
                    o.v.position = UnityObjectToClipPos(float4(local, 1.0));
                    UNITY_TRANSFER_VERTEX_OUTPUT_STEREO(i[0], o);
                    stream.Append(o);
                }
            }

            float4 frag(volume_out i) : SV_Target {
                float4 c = gpu_fragment(i.v);
#ifndef UNITY_COLORSPACE_GAMMA
                c.rgb = GammaToLinearSpace(c.rgb);
#endif
                return float4(c.rgb, 1);
            }
            ENDCG
        }
    }
}
