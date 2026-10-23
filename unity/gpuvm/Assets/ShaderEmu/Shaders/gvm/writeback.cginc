// One of GvmWriteback.shader's passes (devices.shader's GPUWriteback): the including pass defines _GvmComp.
#include "UnityCG.cginc"
#include "../MachineBlit.cginc"
#undef CORES   // (MachineBlit's; this pass reads no core's counts)
Texture2D<uint4> _GvmCtl;
Texture2D<float4> _GpuTarget;
uniform uint _GvmRows;
#define CTL(i) _GvmCtl.Load(int3((i) - 0x700000u, 0, 0))
#define GVM_RAM
#define GPU_STATE _GvmCtl   // (gpu.cginc's RAM functions, which this pass does not call)
#include "../src/gpu.cginc"
// the copy: its first word (RISC-V numbering, (address - 0x80000000) / 4), width, height, row length
bool copy_of(out uint first, out uint width, out uint height, out uint row) {
    uint4 copy = CTL(GPU_COPY);
    first = width = height = row = 0;
    if (copy.r == 0) return false;
    if (copy.r & SUBMIT_INTO) {
        first = ((copy.g + 0x86000000u) & 0x7fffffff) >> 2;
        width = copy.b & 0xffff;
        height = copy.b >> 16;
        row = max(copy.a, width);
    } else {
        uint4 disp = CTL(GPU_DISPLAY);
        first = GPU_PIXELS * 4;
        width = row = disp.g;
        height = disp.b;
    }
    return width != 0 && height != 0;
}
[maxvertexcount(4)]
void geom(triangle blit_v2f corners[3], uint which : SV_PrimitiveID, inout TriangleStream<blit_v2f> s) {
    uint first, width, height, row;
    if (which != 0 || !copy_of(first, width, height, row)) return;
    uint t0 = first / 4 - 0x600000u, t1 = (first + row * (height - 1) + width - 1) / 4 - 0x600000u;
    float y0 = 1 - 2 * (float)(t0 / 8192u) / _GvmRows, y1 = 1 - 2 * (float)(t1 / 8192u + 1) / _GvmRows;
    blit_v2f o;
    o.vertex = float4(-1, y0, 0.5, 1); s.Append(o);
    o.vertex = float4(1, y0, 0.5, 1); s.Append(o);
    o.vertex = float4(-1, y1, 0.5, 1); s.Append(o);
    o.vertex = float4(1, y1, 0.5, 1); s.Append(o);
}
uint4 frag(blit_v2f i) : SV_Target {
    uint index = gvm_texel(uint2(i.vertex.xy)) + 0x600000u, first, width, height, row;
    if (index - 0x600000u >= 0x1e0000u || !copy_of(first, width, height, row)) discard;   // (what a GPU program may own)
    uint word = index * 4 + _GvmComp;
    if (word < first) discard;
    uint p = word - first, y = p / row, x = p - y * row;
    if (x >= width || y >= height) discard;
    uint3 c = (uint3)(_GpuTarget.Load(int3(x, y, 0)).rgb * 255.0 + 0.5);
    return (uint4)((c.r << 16) | (c.g << 8) | c.b);
}
