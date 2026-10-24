// One of GvmCommit.shader's four passes: the including pass defines _Comp.
#include "tick.cginc"
c2f gvm_commit_vert(uint id : SV_VertexID) {
    c2f o = commit_vert(id);
#if GVM_HAS_CORE0
    uint core = id < WCN0 ? 0 : 1 + (id - WCN0) / WCN;   // (c0nb: core 0's WCN0 entries first)
#else
    uint core = id / WCN;
#endif
    if (core >= _NCores) o.vertex = float4(-2, -2, 0, 1);
    return o;
}
