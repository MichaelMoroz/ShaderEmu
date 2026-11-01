"""A payload whose text runs every translated float instruction on edge and random operands.
python gen_ftest.py OUT.bin  -> RAM image from 0x80000000; results stored from RES, one word a test."""
import struct, sys, random

RAM, TEXT, DATA, RES, ROMA = 0x80000000, 0x80400000, 0x80800000, 0x80A00000, 0x40000000
code = []
def emit(w): code.append(w & 0xFFFFFFFF)
def I(op, rd, f3, rs1, imm): emit(((imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op)
def Sx(op, f3, rs1, rs2, imm): emit((((imm >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((imm & 31) << 7) | op)
def R(op, f7, rs2, rs1, f3, rd): emit((f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op)
def lui(rd, v): emit((v & 0xFFFFF000) | (rd << 7) | 0x37)
def li(rd, v):
    v &= 0xFFFFFFFF
    lo = v & 0xFFF; lo = lo - 0x1000 if lo >= 0x800 else lo
    hi = (v - lo) & 0xFFFFFFFF
    lui(rd, hi); I(0x13, rd, 0, rd, lo)
def addi(rd, rs, imm): I(0x13, rd, 0, rs, imm)
def flw(fd, rs, imm): I(0x07, fd, 2, rs, imm)
def fsw(rs, fs, imm): Sx(0x27, 2, rs, fs, imm)
def lw(rd, rs, imm): I(0x03, rd, 2, rs, imm)
def sw(rs, rs2, imm): Sx(0x23, 2, rs, rs2, imm)
def fp(f5, rs2, rs1, rm, rd): R(0x53, f5 << 2, rs2, rs1, rm, rd)
def fma(op, rs3, rs2, rs1, rm, rd): emit((rs3 << 27) | (rs2 << 20) | (rs1 << 15) | (rm << 12) | (rd << 7) | op)
def csrrwi(rd, csr, u): emit((csr << 20) | (u << 15) | (5 << 12) | (rd << 7) | 0x73)
def csrrs(rd, csr, rs): emit((csr << 20) | (rs << 15) | (2 << 12) | (rd << 7) | 0x73)

f = lambda x: struct.unpack('<I', struct.pack('<f', x))[0]
V = [0, 0x80000000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7FA00001, 0xFFC12345, 0x00000001, 0x807FFFFF, 0x00800000,
     f(1.0), f(-1.0), f(0.5), f(1.5), f(2.5), f(-2.5), f(-0.5), f(2147483648.0), f(-2147483648.0), f(2147483520.0),
     f(4294967296.0), f(3e9), f(-3e9), 0x7F7FFFFF, 0xFF7FFFFF, f(1e-30), f(3.0), f(-7.75), f(4294967040.0), f(0.49999997)]
rnd = random.Random(7)
while len(V) < 48: V.append(rnd.getrandbits(32) if len(V) % 3 else f(rnd.uniform(-1e6, 1e6)))
N = len(V)
data = []
for j in range(N): data += [V[j], V[(j * 7 + 3) % N], V[(j * 11 + 5) % N]]

li(10, DATA); li(11, RES); li(12, ROMA); li(13, DATA + 4 * len(data))   # x13: a canary word
tests = []
for f5 in range(4): tests.append(('f', lambda f5=f5: fp(f5, 2, 1, 0, 4)))
tests.append(('f', lambda: fp(11, 0, 1, 0, 4)))
for f3 in range(3): tests.append(('f', lambda f3=f3: fp(4, 2, 1, f3, 4))); tests.append(('f', lambda f3=f3: fp(4, 1, 1, f3, 4)))
for f3 in range(2): tests.append(('f', lambda f3=f3: fp(5, 2, 1, f3, 4)))
for f3 in range(3): tests.append(('x', lambda f3=f3: fp(20, 2, 1, f3, 7)))
for rs2 in range(2):
    for rm in (0, 1, 2, 3, 4): tests.append(('x', lambda rs2=rs2, rm=rm: fp(24, rs2, 1, rm, 7)))
    for frm in (0, 1, 2, 3, 4): tests.append(('xd', frm, lambda rs2=rs2: fp(24, rs2, 1, 7, 7)))
    tests.append(('f', lambda rs2=rs2: fp(26, rs2, 5, 0, 4)))
tests.append(('x', lambda: fp(28, 0, 1, 0, 7))); tests.append(('x', lambda: fp(28, 0, 1, 1, 7)))
tests.append(('f', lambda: fp(30, 0, 5, 0, 4)))
for op in (0x43, 0x47, 0x4B, 0x4F): tests.append(('f', lambda op=op: fma(op, 3, 2, 1, 0, 4)))
tests.append(('f', lambda: fp(0, 4, 4, 0, 4)))    # rd also a source
n = 0
for t in tests:
    for j in range(N):
        o = 12 * j
        flw(1, 10, o); flw(2, 10, o + 4); flw(3, 10, o + 8); lw(5, 10, o); lw(6, 10, o + 4)
        flw(4, 13, 0); addi(7, 0, -1)
        if t[0] == 'xd': csrrwi(0, 2, t[1])
        if n % 2: csrrs(0, 1, 0)                     # half the tests hand over to the interpreter between
        t[-1]()
        if t[0] == 'f': fsw(11, 4, 0)
        else: sw(11, 7, 0)
        addi(11, 11, 4); n += 1
flw(8, 12, 0); fsw(11, 8, 0); addi(11, 11, 4)        # a float load the interpreter makes (the ROM), used after
fp(0, 8, 8, 0, 9); fsw(11, 9, 0); addi(11, 11, 4)
emit(0x0000006F)                                      # j .
img = bytearray(DATA + 4 * len(data) + 4 - RAM)
for i, w in enumerate(code): struct.pack_into('<I', img, TEXT - RAM + 4 * i, w)
for i, w in enumerate(data): struct.pack_into('<I', img, DATA - RAM + 4 * i, w)
struct.pack_into('<I', img, DATA - RAM + 4 * len(data), 0x13572468)
open(sys.argv[1], 'wb').write(img)
print(len(code), 'words of text,', n, 'tests,', N, 'operand sets; results', n + 2, 'words at', hex(RES))
