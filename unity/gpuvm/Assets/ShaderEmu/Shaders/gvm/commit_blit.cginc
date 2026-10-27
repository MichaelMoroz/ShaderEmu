// One of GvmCommitBlit.shader's four commit passes (commit.cginc's points from a geometry shader): the including pass
// defines _Comp.
#include "tick.cginc"
uint _CommitPoints;   // the commit mesh's points: GvmCommit's vertex count
[maxvertexcount(16)]
[instance(32)]
void amp_geo(point amp_d I[1], inout PointStream<c2f> s, uint inst : SV_GSInstanceID) {
    uint pid = I[0].patch;
    uint call = amp_call(I[0], pid, inst);
    [loop] for (uint k = 0; k < 16; k++) {
        uint id = 16 * call + k;
        [branch] if (id < _CommitPoints) {
            c2f o = commit_vert(id);
            uint who = id < WCN0 ? 0 : 1 + (id - WCN0) / 258u;   // (commit_vert's core: WCN is a macro of the static core)
            [branch] if (who < _NCores && o.vertex.x > -1.5) {
                s.Append(o);
                s.RestartStrip();
            }
        }
    }
}
uint4 amp_frag(c2f i) : SV_Target { return commit_frag(i); }
