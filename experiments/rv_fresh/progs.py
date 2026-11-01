"""Test programs for the fresh tick: RAM images, from address 0, in RV32 words."""
import struct
def r(f7, rs2, rs1, f3, rd, op): return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
def i(imm, rs1, f3, rd, op): return ((imm & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op
def s(imm, rs2, rs1, f3): return (((imm >> 5) & 0x7f) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 31) << 7) | 0x23
def b(off, rs2, rs1, f3):
    o = off & 0x1fff
    return (((o >> 12) & 1) << 31) | (((o >> 5) & 63) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (((o >> 1) & 15) << 8) | (((o >> 11) & 1) << 7) | 0x63
def j(off, rd):
    o = off & 0x1fffff
    return (((o >> 20) & 1) << 31) | (((o >> 1) & 0x3ff) << 21) | (((o >> 11) & 1) << 20) | (((o >> 12) & 255) << 12) | (rd << 7) | 0x6f
add = lambda d, a, c: r(0, c, a, 0, d, 0x33)
addi = lambda d, a, im: i(im, a, 0, d, 0x13)
def img(words): return struct.pack('<%dI' % len(words), *words)
def adds():
    "add x1, x1, x2 over a page, then a jump back (x2 = 1 by addi first)"
    w = [addi(2, 0, 1)] + [add(1, 1, 2)] * 1022
    return img(w + [j(-4 * 1022, 0)])
def adds16():
    "14 adds and a loop back: what the old classes' add did"
    w = [addi(2, 0, 1)] + [add(1, 1, 2)] * 15
    return img(w + [j(-4 * 15, 0)])
slli = lambda d, a, sh: i(sh, a, 1, d, 0x13)
srli = lambda d, a, sh: i(sh, a, 5, d, 0x13)
xor = lambda d, a, c: r(0, c, a, 4, d, 0x33)
sub = lambda d, a, c: r(0x20, c, a, 0, d, 0x33)
orr = lambda d, a, c: r(0, c, a, 6, d, 0x33)
andr = lambda d, a, c: r(0, c, a, 7, d, 0x33)
sltu = lambda d, a, c: r(0, c, a, 3, d, 0x33)
bne = lambda a, c, off: b(off, c, a, 1)
def add14():
    "14 add and a loop back by addi and bnez, 16 a trip"
    w = [addi(2, 0, 3), addi(5, 0, 1000)]
    top = len(w)
    w += [add(1, 1, 2)] * 14 + [addi(5, 5, -1), 0]
    w[-1] = bne(5, 0, -4 * (len(w) - 1 - top))
    w += [j(-4 * (len(w) - 1), 0)]
    return img(w)
def alu():
    "the old classes' arithmetic: 15 ops of kinds and a loop back"
    w = [addi(2, 0, 3), addi(7, 0, 5), addi(5, 0, 1000)]
    top = len(w)
    w += [addi(6, 6, 1), xor(7, 7, 6), add(28, 28, 7), slli(29, 28, 3), sub(30, 29, 6), orr(31, 30, 7), i(0x7ff, 6, 7, 6, 0x13), srli(7, 31, 2),
          add(28, 28, 30), i(0x55, 29, 4, 29, 0x13), sltu(30, 28, 29), add(31, 31, 30), slli(6, 6, 1), i(0x401, 7, 5, 7, 0x13), andr(28, 28, 31)]
    w += [addi(5, 5, -1), 0]
    w[-1] = bne(5, 0, -4 * (len(w) - 1 - top))
    w += [j(-4 * (len(w) - 1), 0)]
    return img(w)
def spread():
    "adds whose registers change and whose destination was not just read: x8 to x15 by turns, over a page"
    w = [addi(k, 0, k) for k in range(1, 32)]
    n = len(w)
    while len(w) < 1023:
        k = len(w) % 8
        w.append(add(8 + k, 16 + k, 24 + k))
    return img(w + [j(-4 * 1023 + 4 * n, 0)])
def _ctl(every, far):
    "a taken jump every EVERY instructions, to the next group; FAR texels-worth of nops between groups so the target is not the next texel"
    w = [addi(2, 0, 3), addi(7, 0, 5)]
    start = len(w)
    for g in range(40):
        w += [add(1 + (g & 1), 1, 2)] * (every - 1)
        w += [j(4 * (far + 1), 0)] + [0x13] * far
    w += [j(-4 * (len(w) - start), 0)]
    return img(w)
def jmp4(): return _ctl(4, 0)
def jmp6(): return _ctl(6, 0)
def jmp6far(): return _ctl(6, 9)
def blt():
    "a loop of 5 with a taken branch by bne and a not taken one"
    w = [addi(2, 0, 3), addi(5, 0, 0)]
    w += [addi(5, 5, 1), add(1, 1, 2), b(8, 0, 0, 1), add(1, 1, 5), bne(5, 0, -16)]
    return img(w)
lw = lambda d, a, im: i(im, a, 2, d, 0x03)
lbu = lambda d, a, im: i(im, a, 4, d, 0x03)
lui = lambda d, im: (im << 12) | (d << 7) | 0x37
DATA = 0x2000
def _withdata(w):
    d = [(0x01010101 * (k + 1) + k * 77) & 0xffffffff for k in range(4096)]
    b_ = img(w).ljust(DATA, b'\0') + img(d)
    return b_
def loads():
    "the old classes' loads: 14 lw from a page, addi and bnez"
    w = [lui(10, 2), addi(5, 0, 1000)]
    top = len(w)
    w += [lw(6 + (k % 4), 10, 4 * k) for k in range(14)] + [addi(5, 5, -1), 0]
    w[-1] = bne(5, 0, -4 * (len(w) - 1 - top))
    w += [j(-4 * (len(w) - 1), 0)]
    return _withdata(w)
def mix0():
    "no stores yet: 5 arithmetic, 3 loads, a jump a trip, by a walking pointer"
    w = [lui(10, 2), addi(2, 0, 3), addi(5, 0, 0)]
    top = len(w)
    w += [lw(6, 10, 0), add(7, 6, 2), lw(28, 10, 4), xor(29, 28, 7), addi(5, 5, 8), lbu(30, 10, 9), add(1, 1, 30), i(0xff8, 5, 7, 5, 0x13), add(10, 5, 0), 0]
    w[-2] = add(10, 5, 0)
    w.insert(-1, slli(11, 5, 0))
    w[-1] = j(-4 * (len(w) - 1 - top), 0)
    return _withdata(w)
