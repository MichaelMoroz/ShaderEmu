// EmuUdonCpu's pages of guest RAM into the shader machine's state, in one draw: the stage's row 0
// holds their page numbers, row 1 + s page s's 1,024 words, a word a pixel. A page is 256 texels.
// The words the control pass writes (gpu.h's gpu_control) stay the state's: whole texels in pass 0,
// and texels shared with the guest by a pass each whose ColorMask is the guest's words.
Shader "ShaderEmu/UdonAotScatter"
{
    Properties
    {
        _MainTex ("Stage", 2D) = "black" {}
        _Pages ("Pages in the stage", Float) = 0
        _Send ("1: the GPU's submit word goes too", Float) = 0
    }
    CGINCLUDE
    #include "UnityCG.cginc"

    Texture2D<float4> _MainTex;
    float _Pages, _Send;

    struct v2g { float4 pos : SV_Position; };
    struct g2f { float4 pos : SV_Position; nointerpolation uint2 slot : TEXCOORD0; };   // stage row, the page's first x

    v2g vert(appdata_base v) { v2g o; o.pos = UnityObjectToClipPos(v.vertex); return o; }

    uint word_at(uint x, uint y)
    {
        uint4 b = (uint4)round(_MainTex.Load(int3(x, y, 0)) * 255.0);
        return b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
    }

    // Whose texel t of RAM is: 0 the guest's, 1 the device's, 2 the device's alpha, 3 the device's
    // red and alpha, 4 the guest's red only, 5 the device's red and green.
    uint texel_class(uint t)
    {
        if (t >= 0x100000 && t < 0x100100)
        {
            uint i = t - 0x100000;
            if (i == 0x01) return _Send > 0.5 ? 2 : 3;               // GPU_CTRL: submit, list, count, frames drawn
            if (i == 0x03) return 4;                                 // GPU_CLOCK
            if (i == 0x32) return 2;                                 // COUNT_HEAD
            if (i == 0x23 || i == 0x24) return 5;                    // the sound card's cursor, HOST_OPEN
            if (i == 0x02 || i == 0x07 || (i >= 0x08 && i < 0x10) || i == 0x11 || (i >= 0x25 && i < 0x30)
                || i == 0x31 || (i >= 0x33 && i < 0x3d) || i == 0x3f || i >= 0xc0) return 1;
            return 0;
        }
        if (t >= 0x16b800 && t < 0x16ba00)
        {
            uint i = t - 0x16b800;
            if (i == 0x29) return 4;                                 // the console's words: taken (written is printed here)
            if (i == 0x28 || (i >= 0x2a && i < 0x3a) || (i >= 0x40 && i < 0x180)) return 1;   // CON_HOST, its ring, NET_RX
        }
        return 0;
    }

    // 32 instances of the first triangle, 40 pages each (960 of 1,024 output scalars): 1,280 a draw;
    // `common` passes draw only the two pages the device writes in
    void pages(uint which, uint inst, bool common, inout TriangleStream<g2f> stream)
    {
        if (which != 0) return;
        float rows = 2368.0;   // the state's height, 8,192 texels wide (docs/machine/gpu.md, "Rows of 8192")
        uint count = (uint)_Pages;
        [loop]
        for (uint j = 0; j < 40; j++)
        {
            uint s = inst * 40 + j;
            if (s >= count) return;
            uint page = word_at(s, 0), t = page * 256;
            if (common && page != 0x7000 && page != 0x76b8 && page != 0x76b9) continue;
            float x0 = t % 8192, y = 64 + t / 8192;
            g2f o;
            o.slot = uint2(1 + s, (uint)x0);
            float l = x0 / 4096.0 - 1.0, r = (x0 + 256.0) / 4096.0 - 1.0;
            float top = 1.0 - 2.0 * y / rows, bottom = 1.0 - 2.0 * (y + 1.0) / rows;
            o.pos = float4(l, top, 0.5, 1.0); stream.Append(o);
            o.pos = float4(r, top, 0.5, 1.0); stream.Append(o);
            o.pos = float4(l, bottom, 0.5, 1.0); stream.Append(o);
            o.pos = float4(r, bottom, 0.5, 1.0); stream.Append(o);
            stream.RestartStrip();
        }
    }

    [instance(32)] [maxvertexcount(160)]
    void geom_all(triangle v2g c[3], uint which : SV_PrimitiveID, uint inst : SV_GSInstanceID, inout TriangleStream<g2f> stream) { pages(which, inst, false, stream); }
    [instance(32)] [maxvertexcount(160)]
    void geom_shared(triangle v2g c[3], uint which : SV_PrimitiveID, uint inst : SV_GSInstanceID, inout TriangleStream<g2f> stream) { pages(which, inst, true, stream); }

    uint4 page_texel(g2f i, uint wanted)
    {
        uint2 p = (uint2)i.pos.xy;
        if (texel_class((p.y - 64) * 8192 + p.x) != wanted) discard;
        uint at = (p.x - i.slot.y) * 4;
        return uint4(word_at(at, i.slot.x), word_at(at + 1, i.slot.x), word_at(at + 2, i.slot.x), word_at(at + 3, i.slot.x));
    }
    uint4 frag_guest(g2f i) : SV_Target { return page_texel(i, 0); }
    uint4 frag_rgb(g2f i) : SV_Target { return page_texel(i, 2); }
    uint4 frag_gb(g2f i) : SV_Target { return page_texel(i, 3); }
    uint4 frag_r(g2f i) : SV_Target { return page_texel(i, 4); }
    uint4 frag_ba(g2f i) : SV_Target { return page_texel(i, 5); }
    ENDCG

    SubShader
    {
        ZTest Always Cull Off ZWrite Off
        Pass { CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom_all
            #pragma fragment frag_guest
            ENDCG }
        Pass { ColorMask RGB
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom_shared
            #pragma fragment frag_rgb
            ENDCG }
        Pass { ColorMask GB
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom_shared
            #pragma fragment frag_gb
            ENDCG }
        Pass { ColorMask R
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom_shared
            #pragma fragment frag_r
            ENDCG }
        Pass { ColorMask BA
            CGPROGRAM
            #pragma target 5.0
            #pragma vertex vert
            #pragma geometry geom_shared
            #pragma fragment frag_ba
            ENDCG }
    }
}
