// A tick grown from an empty shader (NOTES.md). Step 1: the state read, a loop that fetches a texel, the state written.
Texture2D<uint4> _Ram : register(t0);
Texture2D<uint4> _S0 : register(t1); Texture2D<uint4> _S1 : register(t2); Texture2D<uint4> _S2 : register(t3); Texture2D<uint4> _S3 : register(t4);
Texture2D<uint4> _S4 : register(t5); Texture2D<uint4> _S5 : register(t6); Texture2D<uint4> _S6 : register(t7); Texture2D<uint4> _S7 : register(t8);
cbuffer C : register(b0) { uint _Budget; };
float4 vert(uint id : SV_VertexID) : SV_Position { return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1); }
struct Out { uint4 o0 : SV_Target0; uint4 o1 : SV_Target1; uint4 o2 : SV_Target2; uint4 o3 : SV_Target3; uint4 o4 : SV_Target4; uint4 o5 : SV_Target5; uint4 o6 : SV_Target6; uint4 o7 : SV_Target7; };
#define RAM(t) _Ram.Load(int3((t) & 2047, (t) >> 11, 0))

Out frag(float4 at : SV_Position) {
    int3 me = int3(at.xy, 0);
    uint4 ctl = _S0.Load(int3(1, 0, 0));
    uint pc = ctl.x, n = 0, seen = 0;   // pc in bytes
    [loop] while (n < _Budget) {
        uint4 t = RAM(pc >> 4);
        seen ^= t[(pc >> 2) & 3];
        pc += 4;
        pc &= 0xfff;
        n++;
    }
    Out r;
    r.o0 = _S0.Load(me); r.o1 = _S1.Load(me); r.o2 = _S2.Load(me); r.o3 = _S3.Load(me); r.o4 = _S4.Load(me); r.o5 = _S5.Load(me); r.o6 = _S6.Load(me); r.o7 = _S7.Load(me);
    if (me.x == 1 && me.y == 0) r.o0 = uint4(pc, ctl.y + n, seen, 0);
    return r;
}
