Shader "ShaderEmu/HolodeckOrigin"
{
    // Drawn once into a one-texel texture when a visitor asks the holodeck to centre: where the
    // program's camera stands in the program's own world now (docs/holodeck.md). GpuHolodeck.shader
    // takes that from every place, and the world then stays where it is while the camera moves.
    Properties
    {
        _State ("Machine state texture", 2D) = "black" {}
    }
    SubShader
    {
        Cull Off ZWrite Off ZTest Always
        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma fragment frag
            #include "UnityCG.cginc"
            Texture2D<uint4> _State;
            #define GPU_STATE _State
            #define GPU_VOLUME
            #include "src/gpu.cginc"

            float4 vert(appdata_base v) : SV_Position { return UnityObjectToClipPos(v.vertex); }

            float4 frag() : SV_Target {
                float3 rx, ry, rz, move;
                if (!volume_view(rx, ry, rz, move)) return 0;
                return float4(-(rx * move.x + ry * move.y + rz * move.z), 1.0);   // camera = rows * world + move, at 0
            }
            ENDCG
        }
    }
}
