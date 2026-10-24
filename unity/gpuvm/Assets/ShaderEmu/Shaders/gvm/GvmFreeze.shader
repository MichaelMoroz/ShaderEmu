Shader "ShaderEmu/GvmFreeze"
{
    // Workers with nothing to do are not run (the harness's freeze, gvm_backend11.cpp, decided here on the card from
    // this pass's RAM instead of a readback some frames old): a blit a pass into a texel pair a core, from the pair the
    // pass before left. A worker the kernel's MC_STATS_ALL says is not running or is asleep, whose mailbox start texel
    // and job word have been as they are for QUIET passes, is frozen: GvmTick.shader copies its block from the set it
    // reads into the set it draws, its write cache empty, in place of its tick. A change to either word, or a count
    // that says it runs, thaws it the next pass. Row 0: (frozen, quiet passes, the job word, bits: 1 runs, 2 asleep, 4 there); row 1: the start texel.
    Properties
    {
        _Ram ("RAM", 2D) = "black" {}
    }
    SubShader
    {
        Tags { "RenderType"="Opaque" "IgnoreProjector"="true" }
        Cull Off
        ZTest Always
        ZWrite Off
        Lighting Off
        Blend Off
        Pass
        {
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex blit_vert
            #pragma fragment freeze_frag
            #include "UnityCG.cginc"
            #include "tick.cginc"
            Texture2D<uint4> _FreezeWas;
            struct b2f { float4 vertex : SV_POSITION; };
            b2f blit_vert(float4 v : POSITION) { b2f o; o.vertex = UnityObjectToClipPos(v); return o; }
            static const uint QUIET = 24;                   // (the harness's kQuiet, in passes here)
            static const uint MBOX_T = GVM_MBOX >> 4;       // the mailboxes: a worker's start texel at MBOX_T + k
            static const uint STATS_T = 0x16ba40u;          // MC_STATS_ALL's bits: texel k / 32, x runs, y asleep
            uint4 freeze_frag(b2f i) : SV_Target {
                uint2 px = uint2(i.vertex.xy);
                uint k = px.x;
                uint4 start = ram(MBOX_T + k), was0 = _FreezeWas.Load(int3(k, 0, 0)), was1 = _FreezeWas.Load(int3(k, 1, 0));
                uint job = ram(MBOX_T + 0x400 + 4 * k).x;
                [branch] if (px.y == 1) return start;
                uint4 bits = ram(STATS_T + k / 32);
                bool runs = ((bits.x >> (k & 31)) & 1) != 0, asleep = ((bits.y >> (k & 31)) & 1) != 0;
                bool same = all(start == was1) && job == was0.z;
                uint quiet = same ? min(was0.y + 1, QUIET) : 0;
                bool frozen = k >= 1 && k < _NCores && (!runs || asleep) && quiet >= QUIET;
                return uint4(frozen ? 1u : 0u, quiet, job, (runs ? 1u : 0u) | (asleep ? 2u : 0u) | (k < _NCores ? 4u : 0u));
            }
            ENDCG
        }
    }
}
