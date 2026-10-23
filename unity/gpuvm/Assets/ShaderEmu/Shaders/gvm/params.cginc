// The tick's PassParams (gpu_vm/tick/STATE.md) as the world's uniforms: a material's numbers are floats, exact to 24
// bits, so TIME arrives in 16-bit halves; the external lines are none; _Comp is the commit pass's own.
uniform uint _Ticks, _RamEnd, _TimeL0, _TimeL1, _TimeH0, _TimeH1, _Player, _Flags, _RamRows, _NCores;
#define _TimeLo ((_TimeL0 & 0xffffu) | (_TimeL1 << 16))
#define _TimeHi ((_TimeH0 & 0xffffu) | (_TimeH1 << 16))
static const uint4 _Ext[2] = { uint4(0, 0, 0, 0), uint4(0, 0, 0, 0) };
#ifndef _Comp
#define _Comp 0u
#endif
