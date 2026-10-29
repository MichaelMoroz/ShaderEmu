"""Runs the tick's bytecode for one pixel of one pass and counts what it executes.

    python3 tools/dxbc_run.py TICK.asm SNAPSHOT CONSTANTS [PIXEL_X PIXEL_Y]

TICK.asm is the tick's pixel shader as dxbc-spirv's dxbc_disasm prints it (docs/world/linux.md),
SNAPSHOT a machine snapshot (rvc_harness --save-state) taken before the pass, CONSTANTS the
pass's constant buffer (RVC_DUMP_CB=FILE while the harness runs that pass). The pixel is one
of the eight-target tick's: (3, 0) by default, whose texels 24 to 31 of core 0's state have
the instruction clock (texel 28, second word), so the pass's emulated instructions are known.

With `--lines FILE` after those it also writes how many times each line of TICK.asm ran (the
count, the line's number among the instructions, its text): what tools/tick_shares.py reads.

Prints how many bytecode instructions ran, how many of them inside the fast loop, how many
emulated instructions the pass was, and the pixel's eight texels (to hold against the state
the harness has after the same pass). The ROM's and the device tree's textures read as zeros:
a pass that reads them is not for this.
"""
import math
import re
import struct
import sys

import numpy as np

M = 0xffffffff
COMP = {'x': 0, 'y': 1, 'z': 2, 'w': 3, 'r': 0, 'g': 1, 'b': 2, 'a': 3}


def f2u(f):
    try:
        return struct.unpack('<I', struct.pack('<f', f))[0]
    except (OverflowError, struct.error):
        return 0x7f800000 if f > 0 else 0xff800000


def u2f(u):
    return struct.unpack('<f', struct.pack('<I', u & M))[0]


def s32(u):
    u &= M
    return u - (1 << 32) if u & 0x80000000 else u


def split_operands(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def number(tok):
    tok = tok.strip()
    if re.match(r'^-?0x[0-9a-fA-F]+$', tok):
        return int(tok, 16) & M, False
    if re.match(r'^-?\d+$', tok):
        return int(tok) & M, False
    low = tok.lower().rstrip('f') if not tok.lower().endswith('inf') else tok.lower()
    if low in ('inf', '+inf'):
        return math.inf, True
    if low == '-inf':
        return -math.inf, True
    if 'nan' in low:
        return math.nan, True
    return float(low), True


class Operand:
    """kind: lit, reg, arr, cb, in, out, tex, null"""

    def __init__(self, text):
        self.text = text
        t = text.strip()
        self.float_literal = None
        self.neg = False
        if t.startswith('-') and re.match(r'^-[a-z]', t):
            # a negated source (float instructions only): the sign bit turned over
            self.neg = True
            t = t[1:]
        if t == 'null':
            self.kind = 'null'
            return
        if t.startswith('('):
            vals = [number(v) for v in t[1:-1].split(',')]
            self.kind = 'lit'
            self.floats = [v for v, isf in vals] if any(isf for _, isf in vals) else None
            self.values = [(f2u(v) if isf else v) for v, isf in vals]
            while len(self.values) < 4:
                self.values.append(self.values[-1])
            return
        m = re.match(r'^[-+]?(0x[0-9a-fA-F]+|\d+(\.\d*)?([eE][-+]?\d+)?f?|inf|nan\w*)$', t, re.I)
        if m:
            v, isf = number(t)
            self.kind = 'lit'
            self.floats = [v] * 4 if isf else None
            self.values = [f2u(v) if isf else v] * 4
            return
        m = re.match(r'^([a-z]+)(\d+)(\[(.*)\])?(\.([xyzwrgba]+))?$', t)
        if not m:
            raise ValueError('operand ' + text)
        name, n, _, index, _, sw = m.groups()
        self.n = int(n)
        self.sw = [COMP[c] for c in sw] if sw else [0, 1, 2, 3]
        self.mask = self.sw
        self.sw4 = self.sw + [self.sw[-1]] * (4 - len(self.sw))
        self.kind = {'r': 'reg', 'x': 'arr', 'cb': 'cb', 'v': 'in', 'o': 'out', 't': 'tex'}[name]
        self.index = None
        if index is not None:
            parts = [p.strip() for p in index.split('+')]
            self.index = []
            for p in parts:
                if re.match(r'^\d+$', p):
                    self.index.append(int(p))
                else:
                    self.index.append(Operand(p))


class Machine:
    def __init__(self, asm, snapshot, constants, pixel):
        text = re.sub(r'\x1b\[[0-9;]*m', '', open(asm, encoding='utf-8', errors='replace').read())
        self.ins = []
        for line in text.splitlines():
            m = re.match(r'\s*(\d+):\s+(\S+)\s*(.*)$', line)
            if not m:
                continue
            op = m.group(2).lower()
            ops = [Operand(o) for o in split_operands(m.group(3))] if not op.startswith('dcl_') else []
            self.ins.append((op, ops, m.group(3)))
        width, rows = (int(v) for v in np.fromfile(snapshot, dtype=np.uint32, count=4)[2:4])   # the header's width and height
        self.state = np.memmap(snapshot, dtype=np.uint32, mode='r', offset=32).reshape(rows, width, 4)
        cb = open(constants, 'rb').read()
        self.cb = [list(struct.unpack_from('<4I', cb, 16 * i)) for i in range(len(cb) // 16)]
        self.pixel = pixel
        self.regs = {}
        self.arrays = {}
        self.out = {}
        self.link()

    def link(self):
        """Where every if, loop and switch ends."""
        self.jump = {}
        stack = []
        for i, (op, ops, _) in enumerate(self.ins):
            if op in ('if_nz', 'if_z'):
                stack.append(['if', i, None])
            elif op == 'else':
                stack[-1][2] = i
            elif op == 'endif':
                _, start, els = stack.pop()
                self.jump[start] = (els, i)
                if els is not None:
                    self.jump[els] = i
            elif op == 'loop':
                stack.append(['loop', i, []])
            elif op == 'endloop':
                _, start, breaks = stack.pop()
                self.jump[i] = start
                self.jump[start] = i
                for b in breaks:
                    self.jump[b] = i
            elif op == 'switch':
                stack.append(['switch', i, [], []])   # breaks, cases
            elif op in ('case', 'default'):
                for s in reversed(stack):
                    if s[0] == 'switch':
                        s[3].append((op, i))
                        break
            elif op == 'endswitch':
                _, start, breaks, cases = stack.pop()
                self.jump[start] = (cases, i)
                for b in breaks:
                    self.jump[b] = i
            elif op in ('break', 'breakc_nz', 'breakc_z'):
                for s in reversed(stack):
                    if s[0] in ('loop', 'switch'):
                        s[2].append(i)
                        break
            elif op in ('continue', 'continuec_nz', 'continuec_z'):
                for s in reversed(stack):
                    if s[0] == 'loop':
                        self.jump[i] = s[1]
                        break
        # the fast loop: the largest loop inside the outer one
        depth, loops, st = 0, [], []
        for i, (op, _, _) in enumerate(self.ins):
            if op == 'loop':
                st.append(i)
            elif op == 'endloop':
                s = st.pop()
                loops.append((len(st), s, i))
        inner = max((l for l in loops if l[0] == 1), key=lambda l: l[2] - l[1])
        self.fast = (inner[1], inner[2])

    # ---- values ----
    def reg(self, n):
        r = self.regs.get(n)
        if r is None:
            r = self.regs[n] = [0, 0, 0, 0]
        return r

    def index_of(self, o):
        total = 0
        for part in o.index:
            total += part if isinstance(part, int) else self.read(part)[0]
        return total & M

    def read(self, o):
        """The operand's four components, as its swizzle orders them."""
        if o.neg:
            return [v ^ 0x80000000 for v in self.read_plain(o)]
        return self.read_plain(o)

    def read_plain(self, o):
        k = o.kind
        if k == 'lit':
            return o.values
        if k == 'reg':
            r = self.reg(o.n)
            s = o.sw4
            return [r[s[0]], r[s[1]], r[s[2]], r[s[3]]]
        if k == 'arr':
            a = self.arrays.setdefault(o.n, {})
            r = a.get(self.index_of(o), [0, 0, 0, 0])
            s = o.sw4
            return [r[s[0]], r[s[1]], r[s[2]], r[s[3]]]
        if k == 'cb':
            i = self.index_of(o)
            r = self.cb[i] if i < len(self.cb) else [0, 0, 0, 0]
            s = o.sw4
            return [r[s[0]], r[s[1]], r[s[2]], r[s[3]]]
        if k == 'in':
            r = [f2u(self.pixel[0] + 0.5), f2u(self.pixel[1] + 0.5), 0, f2u(1.0)]
            s = o.sw4
            return [r[s[0]], r[s[1]], r[s[2]], r[s[3]]]
        raise ValueError('read ' + o.text)

    def write(self, o, values):
        """values[i] goes to component i of the destination, for the components in its mask."""
        k = o.kind
        if k == 'null':
            return
        if k == 'reg':
            r = self.reg(o.n)
        elif k == 'arr':
            a = self.arrays.setdefault(o.n, {})
            i = self.index_of(o)
            r = a.get(i)
            if r is None:
                r = a[i] = [0, 0, 0, 0]
        elif k == 'out':
            r = self.out.setdefault(o.n, [0, 0, 0, 0])
        else:
            raise ValueError('write ' + o.text)
        for c in o.mask:
            r[c] = values[c] & M

    def load(self, tex, coord):
        if tex != 0:
            return [0, 0, 0, 0]
        x, y = coord[0], coord[1]
        if x >= self.state.shape[1] or y >= self.state.shape[0]:
            return [0, 0, 0, 0]
        return [int(v) for v in self.state[y, x]]

    # ---- running ----
    def run(self):
        ins, jump = self.ins, self.jump
        count = fast = trips = 0
        hits = self.hits = [0] * len(ins)
        f0, f1 = self.fast
        pc = 0
        n = len(ins)
        T = M
        while pc < n:
            op, o, raw = ins[pc]
            if op.startswith('dcl_'):
                pc += 1
                continue
            if op in ('else',):
                pc = jump[pc]
                continue
            if op in ('endif', 'case', 'default', 'endswitch'):
                pc += 1
                continue
            count += 1
            hits[pc] += 1
            if f0 <= pc <= f1:
                fast += 1
            if op == 'loop':
                count -= 1
                hits[pc] -= 1
                if f0 <= pc <= f1:
                    fast -= 1
                pc += 1
                continue
            if op == 'endloop':
                if pc == f1:
                    trips += 1
                pc = jump[pc] + 1
                continue
            if op == 'ret':
                break
            if op in ('if_nz', 'if_z'):
                v = self.read(o[0])[0]
                taken = (v != 0) if op == 'if_nz' else (v == 0)
                els, end = jump[pc]
                pc = pc + 1 if taken else ((els + 1) if els is not None else end)
                continue
            if op == 'break':
                pc = jump[pc] + 1
                continue
            if op in ('breakc_nz', 'breakc_z'):
                v = self.read(o[0])[0]
                if (v != 0) == (op == 'breakc_nz'):
                    pc = jump[pc] + 1
                else:
                    pc += 1
                continue
            if op == 'continue':
                trips += jump[pc] == f0
                pc = jump[pc] + 1
                continue
            if op in ('continuec_nz', 'continuec_z'):
                v = self.read(o[0])[0]
                if (v != 0) == (op == 'continuec_nz'):
                    trips += jump[pc] == f0
                    pc = jump[pc] + 1
                else:
                    pc += 1
                continue
            if op == 'switch':
                v = self.read(o[0])[0]
                cases, end = jump[pc]
                target = None
                for kind, at in cases:
                    if kind == 'case' and (number(ins[at][2])[0] == v):
                        target = at
                        break
                if target is None:
                    for kind, at in cases:
                        if kind == 'default':
                            target = at
                pc = target if target is not None else end
                continue
            # ---- data ----
            if op == 'mov':
                self.write(o[0], self.read(o[1]))
            elif op == 'movc':
                c, a, b = self.read(o[1]), self.read(o[2]), self.read(o[3])
                self.write(o[0], [a[i] if c[i] else b[i] for i in range(4)])
            elif op in ('and', 'or', 'xor', 'iadd', 'ishl', 'ushr', 'ishr', 'ieq', 'ine', 'ult', 'uge', 'ilt', 'ige',
                        'imax', 'imin', 'umax', 'umin'):
                a, b = self.read(o[1]), self.read(o[2])
                if op == 'and':
                    r = [a[i] & b[i] for i in range(4)]
                elif op == 'or':
                    r = [a[i] | b[i] for i in range(4)]
                elif op == 'xor':
                    r = [a[i] ^ b[i] for i in range(4)]
                elif op == 'iadd':
                    r = [(a[i] + b[i]) & M for i in range(4)]
                elif op == 'ishl':
                    r = [(a[i] << (b[i] & 31)) & M for i in range(4)]
                elif op == 'ushr':
                    r = [a[i] >> (b[i] & 31) for i in range(4)]
                elif op == 'ishr':
                    r = [(s32(a[i]) >> (b[i] & 31)) & M for i in range(4)]
                elif op == 'ieq':
                    r = [T if a[i] == b[i] else 0 for i in range(4)]
                elif op == 'ine':
                    r = [T if a[i] != b[i] else 0 for i in range(4)]
                elif op == 'ult':
                    r = [T if a[i] < b[i] else 0 for i in range(4)]
                elif op == 'uge':
                    r = [T if a[i] >= b[i] else 0 for i in range(4)]
                elif op == 'ilt':
                    r = [T if s32(a[i]) < s32(b[i]) else 0 for i in range(4)]
                elif op == 'ige':
                    r = [T if s32(a[i]) >= s32(b[i]) else 0 for i in range(4)]
                elif op == 'imax':
                    r = [max(s32(a[i]), s32(b[i])) & M for i in range(4)]
                elif op == 'imin':
                    r = [min(s32(a[i]), s32(b[i])) & M for i in range(4)]
                elif op == 'umax':
                    r = [max(a[i], b[i]) for i in range(4)]
                else:
                    r = [min(a[i], b[i]) for i in range(4)]
                self.write(o[0], r)
            elif op == 'not':
                a = self.read(o[1])
                self.write(o[0], [(~v) & M for v in a])
            elif op == 'ineg':
                a = self.read(o[1])
                self.write(o[0], [(-v) & M for v in a])
            elif op in ('imul', 'umul'):
                a, b = self.read(o[2]), self.read(o[3])
                if op == 'imul':
                    p = [s32(a[i]) * s32(b[i]) for i in range(4)]
                else:
                    p = [a[i] * b[i] for i in range(4)]
                self.write(o[0], [(v >> 32) & M for v in p])
                self.write(o[1], [v & M for v in p])
            elif op == 'udiv':
                a, b = self.read(o[2]), self.read(o[3])
                self.write(o[0], [(a[i] // b[i]) if b[i] else M for i in range(4)])
                self.write(o[1], [(a[i] % b[i]) if b[i] else M for i in range(4)])
            elif op == 'ld':
                c = self.read(o[1])
                t = self.load(o[2].n, c)
                s = o[2].sw4
                self.write(o[0], [t[s[0]], t[s[1]], t[s[2]], t[s[3]]])
            elif op == 'resinfo_uint':
                t = [self.state.shape[1], self.state.shape[0], 0, 1]
                s = o[2].sw4
                self.write(o[0], [t[s[0]], t[s[1]], t[s[2]], t[s[3]]])
            elif op in ('add', 'mul', 'div', 'lt', 'ge', 'eq', 'ne', 'min', 'max'):
                a = [u2f(v) for v in self.read(o[1])]
                b = [u2f(v) for v in self.read(o[2])]
                if op == 'add':
                    r = [f2u(a[i] + b[i]) for i in range(4)]
                elif op == 'mul':
                    r = [f2u(a[i] * b[i]) for i in range(4)]
                elif op == 'div':
                    r = [f2u(a[i] / b[i]) if b[i] != 0 else f2u(math.inf) for i in range(4)]
                elif op == 'lt':
                    r = [T if a[i] < b[i] else 0 for i in range(4)]
                elif op == 'ge':
                    r = [T if a[i] >= b[i] else 0 for i in range(4)]
                elif op == 'eq':
                    r = [T if a[i] == b[i] else 0 for i in range(4)]
                elif op == 'ne':
                    r = [T if a[i] != b[i] else 0 for i in range(4)]
                elif op == 'min':
                    r = [f2u(min(a[i], b[i])) for i in range(4)]
                else:
                    r = [f2u(max(a[i], b[i])) for i in range(4)]
                self.write(o[0], r)
            elif op == 'mad':
                a = [u2f(v) for v in self.read(o[1])]
                b = [u2f(v) for v in self.read(o[2])]
                c = [u2f(v) for v in self.read(o[3])]
                self.write(o[0], [f2u(a[i] * b[i] + c[i]) for i in range(4)])
            elif op in ('sqrt', 'round_ni', 'round_z', 'round_pi', 'round_ne', 'ftou', 'ftoi', 'utof', 'itof'):
                a = self.read(o[1])
                if op == 'utof':
                    r = [f2u(float(v)) for v in a]
                elif op == 'itof':
                    r = [f2u(float(s32(v))) for v in a]
                else:
                    f = [u2f(v) for v in a]
                    if op == 'sqrt':
                        r = [f2u(math.sqrt(v)) if v >= 0 else 0x7fc00000 for v in f]
                    elif op == 'round_ni':
                        r = [f2u(math.floor(v)) if math.isfinite(v) else f2u(v) for v in f]
                    elif op == 'round_pi':
                        r = [f2u(math.ceil(v)) if math.isfinite(v) else f2u(v) for v in f]
                    elif op == 'round_z':
                        r = [f2u(math.trunc(v)) if math.isfinite(v) else f2u(v) for v in f]
                    elif op == 'round_ne':
                        r = [f2u(round(v)) if math.isfinite(v) else f2u(v) for v in f]
                    elif op == 'ftou':
                        r = [min(max(int(v), 0), M) if math.isfinite(v) else 0 for v in f]
                    else:
                        r = [min(max(int(v), -(1 << 31)), (1 << 31) - 1) & M if math.isfinite(v) else 0 for v in f]
                self.write(o[0], r)
            elif op in ('ftod', 'dtof', 'dtou', 'dmul', 'ddiv', 'dadd'):
                self.double(op, o)
            else:
                raise NotImplementedError('%s %s' % (op, raw))
            pc += 1
        return count, fast, trips

    def get_double(self, o, i):
        """Double i of a source: a literal's value, or two components (low, high) of a register."""
        if o.kind == 'lit':
            if o.floats:
                return o.floats[min(i, len(o.floats) - 1)]
            return float(o.values[min(i, 3)])
        v = self.read(o)
        return struct.unpack('<d', struct.pack('<II', v[2 * i], v[2 * i + 1]))[0]

    def put_double(self, o, values):
        r = [0, 0, 0, 0]
        for i, d in enumerate(values[:2]):
            lo, hi = struct.unpack('<II', struct.pack('<d', d))
            r[2 * i], r[2 * i + 1] = lo, hi
        self.write(o, r)

    def double(self, op, o):
        if op == 'ftod':
            f = [u2f(v) for v in self.read(o[1])]
            self.put_double(o[0], [f[0], f[1]])
        elif op == 'dtof':
            self.write(o[0], [f2u(self.get_double(o[1], 0))] * 4)
        elif op == 'dtou':
            d = self.get_double(o[1], 0)
            self.write(o[0], [min(max(int(d), 0), M) if math.isfinite(d) else 0] * 4)
        else:
            a = [self.get_double(o[1], 0), self.get_double(o[1], 1)]
            b = [self.get_double(o[2], 0), self.get_double(o[2], 1)]
            if op == 'dmul':
                r = [a[0] * b[0], a[1] * b[1]]
            elif op == 'dadd':
                r = [a[0] + b[0], a[1] + b[1]]
            else:
                r = [a[0] / b[0] if b[0] else math.inf, a[1] / b[1] if b[1] else math.inf]
            self.put_double(o[0], r)


def main():
    args = sys.argv[1:]
    lines = None
    if '--lines' in args:
        at = args.index('--lines')
        lines = args[at + 1]
        del args[at:at + 2]
    asm, snapshot, constants = args[0:3]
    pixel = (int(args[3]), int(args[4])) if len(args) > 4 else (3, 0)
    m = Machine(asm, snapshot, constants, pixel)
    count, fast, trips = m.run()
    if lines:
        with open(lines, 'w') as f:
            f.write('# fast loop %d %d\n' % m.fast)
            for i, (op, _, raw) in enumerate(m.ins):
                f.write('%d\t%d\t%s %s\n' % (m.hits[i], i, op, raw))
    before = int(m.state[0, 28, 1])
    after = m.out.get(4, [0, 0, 0, 0])[1] if pixel == (3, 0) else None
    print('%s: %d bytecode instructions for the pixel, %d of them in the fast loop (%d trips of it)' % (asm, count, fast, trips))
    if after is not None:
        emulated = (after - before) & M
        print('the pass: %d emulated instructions (the clock %d to %d)' % (emulated, before, after))
        if emulated:
            print('bytecode instructions for an emulated one: %.2f in all, %.2f in the fast loop' % (count / emulated, fast / emulated))
    for k in sorted(m.out):
        print('  o%d = %s' % (k, ' '.join('%08x' % v for v in m.out[k])))


main()
