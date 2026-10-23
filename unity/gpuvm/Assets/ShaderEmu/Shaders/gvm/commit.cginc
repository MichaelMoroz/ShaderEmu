// One of GvmCommit.shader's four passes: the including pass defines _Comp.
#include "tick.cginc"
c2f gvm_commit_vert(uint id : SV_VertexID) {
    c2f o = commit_vert(id);
    if (id / WCN >= _NCores) o.vertex = float4(-2, -2, 0, 1);
    return o;
}
