#!/usr/bin/env python3
"""What a test program's registers should be after N instructions: python sim.py PROG N (RV32I, with RAM from the image)."""
import struct, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import progs
M = 0xffffffff
def sx(v, b): return (v & ((1 << b) - 1)) - ((v >> (b - 1) & 1) << b)
def sim(img, n):
    mem = bytearray(img) + bytearray(1 << 20); x = [0] * 32; pc = 0
    for _ in range(n):
        w = struct.unpack_from('<I', mem, pc)[0]; op = w & 0x7f; f3 = w >> 12 & 7; rd = w >> 7 & 31; a = x[w >> 15 & 31]; b = x[w >> 20 & 31]; nx = pc + 4; v = None
        if op in (0x13, 0x33):
            y = b if op == 0x33 else sx(w >> 20, 12) & M; s = y & 31; alt = w >> 30 & 1
            sa, sy = sx(a, 32), sx(y, 32)
            v = [((a - y) if (op == 0x33 and alt) else (a + y)), a << s, sa < sy, a < y, a ^ y, (sa >> s) if alt else (a >> s), a | y, a & y][f3]
        elif op == 0x37: v = w & 0xfffff000
        elif op == 0x17: v = pc + (w & 0xfffff000)
        elif op == 0x6f:
            v = pc + 4; nx = (pc + sx((w >> 31 << 20) | (w & 0xff000) | ((w >> 20 & 1) << 11) | (w >> 20 & 0x7fe), 21)) & M
        elif op == 0x67: v = pc + 4; nx = (a + sx(w >> 20, 12)) & M
        elif op == 0x63:
            t = {0: a == b, 1: a != b, 4: sx(a, 32) < sx(b, 32), 5: sx(a, 32) >= sx(b, 32), 6: a < b, 7: a >= b}[f3]
            if t: nx = (pc + sx((w >> 31 << 12) | ((w >> 7 & 1) << 11) | (w >> 20 & 0x7e0) | (w >> 7 & 0x1e), 13)) & M
        elif op == 0x03:
            at = (a + sx(w >> 20, 12)) & M
            v = {0: sx(mem[at], 8) & M, 1: sx(struct.unpack_from('<H', mem, at)[0], 16) & M, 2: struct.unpack_from('<I', mem, at)[0], 4: mem[at], 5: struct.unpack_from('<H', mem, at)[0]}[f3]
        elif op == 0x23:
            at = (a + sx((w >> 25 << 5) | (w >> 7 & 31), 12)) & M
            if f3 == 0: mem[at] = b & 255
            elif f3 == 1: struct.pack_into('<H', mem, at, b & 0xffff)
            else: struct.pack_into('<I', mem, at, b)
        else: raise SystemExit('bad word %08x at %x' % (w, pc))
        if v is not None and rd: x[rd] = int(v) & M
        pc = nx
    return x, pc
if __name__ == '__main__':
    x, pc = sim(getattr(progs, sys.argv[1])(), int(sys.argv[2]))
    print('pc=%08x' % pc)
    for k in range(0, 8, 1): print('texel %d: %08x %08x %08x %08x' % (k, *x[4 * k:4 * k + 4]))
