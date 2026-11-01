import sys
NB0=int(sys.argv[1]); name=sys.argv[2]
WCN0=8*NB0+2; TEX=40+WCN0+(WCN0+3)//4
s=open('tick.hlsl',newline='').read()
nl='\r\n' if '\r\n' in s else '\n'
head="static const uint NB = 21;           // buckets of four entries a table"+nl+"static const uint WCN = 170;         // write cache entries: 8 * NB in buckets, two overflow slots, then a sink"+nl+"static const uint WCS = WCN - 2;"
assert head in s
pix=(TEX+7)//8; assert pix%32==0 or True
rows=(pix+31)//32*8
new=f"""// core 0's own write cache, its own compile (-D CORE0) and draw: a column 4 wide, its warps down the first rows
#ifndef CORE0
static const uint NB = 21;           // buckets of four entries a table
static const uint WCN = 170;         // write cache entries: 8 * NB in buckets, two overflow slots, then a sink
static const uint NTEX = 253;
static const uint SPIX = 32;
#else
static const uint NB = {NB0};
static const uint WCN = {WCN0};
static const uint NTEX = {TEX};
static const uint SPIX = {pix};
#endif
static const uint WCN0 = {WCN0};
static const uint NTEX0 = {TEX};
static const uint WCS = WCN - 2;""".replace('\n',nl)
s=s.replace(head,new)
s=s.replace("static const uint NTEX = 253;"+nl+"static const uint SPIX = 32;"+nl+"static const uint WAITW","static const uint T_WT0 = T_WC + WCN0;"+nl+"static const uint WAITW")
a="int3 sat(uint j, uint c) { return int3(slot_of(c % 256u) * 4u + (j / 8u), (c / 256u) * 8u + (j % 8u), 0); }"
assert a in s
s=s.replace(a,a+nl+"""#ifdef CORE0
int3 sat0(uint j, uint c) { uint k = j % 32u; return int3(k / 8u, (j / 32u) * 8u + k % 8u, 0); }
#define sat(j, c) sat0(j, c)
#endif
int3 sat_any(uint j, uint c) { uint k = j % 32u; return c == 0 ? int3(k / 8u, (j / 32u) * 8u + k % 8u, 0) : int3(slot_of(c % 256u) * 4u + (j / 8u), (c / 256u) * 8u + (j % 8u), 0); }""".replace('\n',nl))
a="    uint rx = p.x % 4u, ry = p.y % 8u;"+nl+"    core = (p.y / 8u) * 256u + core_at(p.x / 4u);"+nl+"    uint j = rx * 8u + ry;"
assert a in s
s=s.replace(a,"    uint rx = p.x % 4u, ry = p.y % 8u;"+nl+"#ifdef CORE0"+nl+"    core = 0;"+nl+"    uint j = (p.y / 8u) * 32u + rx * 8u + ry;"+nl+"#else"+nl+"    core = (p.y / 8u) * 256u + core_at(p.x / 4u);"+nl+"    uint j = rx * 8u + ry;"+nl+"#endif")
a="[branch] if (core >= _NCores || j >= SPIX || ry >= 8u)"; assert a in s
a="    int3 at = sat(t >> 3, c);"+nl+"    uint4 v0 = (t & 1)"
assert a in s
s=s.replace(a,"    int3 at = sat_any(t >> 3, c);"+nl+"    uint4 v0 = (t & 1)")
a="    uint c = id / WCN, j = id % WCN;"+nl+"    uint tg = pick(state_any(T_WT + (j >> 2), c), j & 3), tex"
assert a in s
s=s.replace(a,"    uint c = id < WCN0 ? 0 : 1 + (id - WCN0) / WCN, j = id < WCN0 ? id : (id - WCN0) % WCN;"+nl+"    uint tg = pick(state_any((c == 0 ? T_WT0 : T_WT) + (j >> 2), c), j & 3), tex")
open(name,'w',newline='').write(s)
print(name,'entries',WCN0,'texels',TEX,'rows',rows)
