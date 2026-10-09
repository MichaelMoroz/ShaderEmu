"""Turns hot straight-line traces of guest code into HLSL for the tick's fast loop (an experiment).

    python tools/rv_trace.py CODE.BIN BASE PC.LOG START[:MAX] [START[:MAX] ...] > experiments/rvc_opt/src/blocks.h

CODE.BIN is the code as it sits in memory and BASE the address of its first byte (our kernel:
linux/prebuilt/Image 0xc0000000); PC.LOG is a --pc-log profile of it (tools/pc_hot.py says how
to take one), used to choose which way each conditional branch of a trace usually goes.

A trace starts at START and follows the code: through jal to its target, through a conditional
branch the way the profile says it mostly goes, up to MAX instructions (64 if not given) or to
the first instruction it cannot translate or an indirect jump. Registers live in locals for the
length of the trace; every way out of it (a branch that goes the other way, a load or store
the fast path would not take, a full write cache, the end) writes the changed ones back and
leaves pc and the instruction count as the interpreter would have. fast_run (cpu.h, under
#ifdef USE_BLOCKS) leaves its loop where BLOCK_STARTS_HERE says a trace begins and runs
FAST_BLOCKS after it: the traces' code inside the loop made every instruction slower.
"""
import os
import sys
import numpy as np

REG = ['zero', 'ra', 'sp', 'gp', 'tp', 't0', 't1', 't2', 's0', 's1', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7',
       's2', 's3', 's4', 's5', 's6', 's7', 's8', 's9', 's10', 's11', 't3', 't4', 't5', 't6']


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def u(v):
    return '0x%08xu' % (v & 0xffffffff)


class Trace:
    def __init__(self, code, base, weight, start, limit):
        self.code, self.base, self.weight = code, base, weight
        self.start = start
        self.out = []
        self.read = set()      # registers loaded into locals so far
        self.dirty = []        # registers written, in order
        self.count = 0
        self.last_word = 0
        self.uid = 0
        pc = start
        while self.count < limit:
            nxt = self.instr(pc)
            if nxt is None:
                break
            pc = nxt
        self.end_pc = pc
        self.exit_at(pc, self.count, final=True)

    def word(self, pc):
        return int(self.code[(pc - self.base) // 4])

    # --- registers
    def get(self, r):
        if r == 0:
            return '0u'
        if r not in self.read and r not in self.dirty:
            self.read.add(r)
            self.out.append('uint x%d = xr[%d];' % (r, r))
        return 'x%d' % r

    def set(self, r, expr):
        if r == 0:
            return
        if r in self.read or r in self.dirty:
            self.out.append('x%d = %s;' % (r, expr))
        else:
            self.out.append('uint x%d = %s;' % (r, expr))
        if r not in self.dirty:
            self.dirty.append(r)

    def writeback(self):
        return ' '.join('xr[%d] = x%d;' % (r, r) for r in self.dirty)

    def exit_code(self, pc, count):
        s = self.writeback()
        s += ' cpu.pc = %s; blk_n = %d;' % (u(pc), count)
        if count:
            s += ' cpu.debug_last_ins = %s;' % u(self.last_word)
        return s

    def exit_if(self, cond, pc, count):
        self.out.append('if (%s) { %s break; }' % (cond, self.exit_code(pc, count)))

    def exit_at(self, pc, count, final=False):
        self.out.append('%s' % self.exit_code(pc, count))

    # --- one instruction; returns the next pc of the trace, or None to end the trace before it
    def instr(self, pc):
        if not (self.base <= pc < self.base + 4 * len(self.code)):
            return None
        w = self.word(pc)
        o, rd, f3, r1, r2, f7 = w & 0x7f, (w >> 7) & 31, (w >> 12) & 7, (w >> 15) & 31, (w >> 20) & 31, w >> 25
        self.uid += 1
        n = self.uid
        done = lambda nxt: self.finish(w, nxt)
        if o == 0x13:
            a, imm = self.get(r1), sx(w >> 20, 12)
            sh = r2
            if f3 == 0: e = '%s + %s' % (a, u(imm))
            elif f3 == 1:
                if f7 != 0: return None
                e = '%s << %d' % (a, sh)
            elif f3 == 2: e = '(int)%s < (int)%s ? 1u : 0u' % (a, u(imm))
            elif f3 == 3: e = '%s < %s ? 1u : 0u' % (a, u(imm))
            elif f3 == 4: e = '%s ^ %s' % (a, u(imm))
            elif f3 == 5:
                if f7 == 0: e = '%s >> %d' % (a, sh)
                elif f7 == 0x20: e = '(uint)((int)%s >> %d)' % (a, sh)
                else: return None
            elif f3 == 6: e = '%s | %s' % (a, u(imm))
            else: e = '%s & %s' % (a, u(imm))
            self.set(rd, e)
            return done(pc + 4)
        if o == 0x33:
            a, b = self.get(r1), self.get(r2)
            if f7 == 0:
                e = ['%s + %s', '%s << (%s & 31)', '(int)%s < (int)%s ? 1u : 0u', '%s < %s ? 1u : 0u', '%s ^ %s',
                     '%s >> (%s & 31)', '%s | %s', '%s & %s'][f3] % (a, b)
            elif f7 == 0x20 and f3 == 0: e = '%s - %s' % (a, b)
            elif f7 == 0x20 and f3 == 5: e = '(uint)((int)%s >> (%s & 31))' % (a, b)
            elif f7 == 1 and f3 == 0: e = '%s * %s' % (a, b)
            else: return None
            self.set(rd, e)
            return done(pc + 4)
        if o == 0x37:
            self.set(rd, u(w & 0xfffff000))
            return done(pc + 4)
        if o == 0x17:
            self.set(rd, u(pc + (w & 0xfffff000)))
            return done(pc + 4)
        if o == 0x03:
            if f3 not in (0, 1, 2, 4, 5) or os.environ.get('RVT_NO_LOAD'): return None
            size = 1 << (f3 & 3)
            self.out.append('uint va%d = %s + %s;' % (n, self.get(r1), u(sx(w >> 20, 12))))
            self.out.append('FAST_XL(xl_ident_d, MMU_ACCESS_READ, tlb_r_vpn, tlb_r_page, va%d, ok%d, pa%d)' % (n, n, n))
            self.exit_if('!(va%d != 0 && ok%d && (pa%d & 0x80000000) != 0 && (pa%d & 3) + %d <= 4 && (pa%d & 0x7ffffffc) < RAM_MAX)'
                         % (n, n, n, n, size, n), pc, self.count)
            self.out.append('uint mv%d = mem_get_cached_or_tex(pa%d & 0x7ffffffc) >> ((pa%d & 3) * 8);' % (n, n, n))
            e = {0: 'sign_extend(mv%d & 0xff, 8)', 1: 'sign_extend(mv%d & 0xffff, 16)', 2: 'mv%d', 4: 'mv%d & 0xff', 5: 'mv%d & 0xffff'}[f3] % n
            self.set(rd, e)
            return done(pc + 4)
        if o == 0x23:
            if f3 > 2 or os.environ.get('RVT_NO_STORE'): return None
            size = 1 << f3
            imm = sx(((w >> 25) << 5) | rd, 12)
            self.out.append('uint va%d = %s + %s;' % (n, self.get(r1), u(imm)))
            src = self.get(r2)
            self.out.append('FAST_XL(xl_ident_d, MMU_ACCESS_WRITE, tlb_w_vpn, tlb_w_page, va%d, ok%d, pa%d)' % (n, n, n))
            self.exit_if('!(ok%d && (pa%d & 0x80000000) != 0 && (pa%d & 3) + %d <= 4)' % (n, n, n, size), pc, self.count)
            mask = {0: '0xffu', 1: '0xffffu', 2: '0xffffffffu'}[f3]
            self.out.append('mem_set_ram(pa%d & 0x7ffffffc, %s << ((pa%d & 3) * 8), %s << ((pa%d & 3) * 8));' % (n, src, n, mask, n))
            nxt = done(pc + 4)
            self.exit_if('cpu.stall', pc + 4, self.count)
            return nxt
        if o == 0x63:
            if f3 in (2, 3): return None
            a, b = self.get(r1), self.get(r2)
            cond = {0: '%s == %s', 1: '%s != %s', 4: '(int)%s < (int)%s', 5: '(int)%s >= (int)%s', 6: '%s < %s', 7: '%s >= %s'}[f3] % (a, b)
            imm = sx(((w >> 31) << 12) | (((w >> 7) & 1) << 11) | (((w >> 25) & 0x3f) << 5) | (((w >> 8) & 0xf) << 1), 13)
            target, fall = pc + imm, pc + 4
            # The instruction after a branch is run when the branch is not taken and (unless something
            # else jumps there) only then: it tells which way the branch mostly goes.
            taken = self.weight.get(fall, 0) < 0.6 * self.weight.get(pc, 0)
            self.finish(w, None)
            if taken:
                self.exit_if('!(%s)' % cond, fall, self.count)
                return target
            self.exit_if(cond, target, self.count)
            return fall
        if o == 0x6f:
            imm = sx(((w >> 31) << 20) | (((w >> 12) & 0xff) << 12) | (((w >> 20) & 1) << 11) | (((w >> 21) & 0x3ff) << 1), 21)
            self.set(rd, u(pc + 4))
            return done(pc + imm)
        return None    # jalr, system, atomics, divisions: the interpreter's

    def finish(self, w, nxt):
        self.count += 1
        self.last_word = w
        return nxt


def main():
    args = sys.argv[1:]
    code = np.fromfile(args[0], dtype=np.uint8)
    code = code[:len(code) // 4 * 4].view(np.uint32)
    base = int(args[1], 0)
    samples = np.fromfile(args[2], dtype=np.uint32).reshape(-1, 2)
    weight = {}
    for pc, c in samples:
        weight[int(pc)] = weight.get(int(pc), 0) + int(c)
    traces = []
    for spec in args[3:]:
        start, _, limit = spec.partition(':')
        traces.append(Trace(code, base, weight, int(start, 16), int(limit) if limit else 64))
    print('// Generated by tools/rv_trace.py: %s' % ' '.join(args[3:]))
    print('// Each trace: where it starts, how many instructions it is, where it ends when nothing leaves it early.')
    for t in traces:
        print('//   %08x: %d instructions, to %08x' % (t.start, t.count, t.end_pc))
    # the fast loop only finds out that a trace starts here; the trace itself runs outside the loop
    print('#define BLOCK_STARTS_HERE (%s)' % ' || '.join('(cpu.pc == %s && n + %d <= budget)' % (u(t.start), t.count) for t in traces))
    print('#define FAST_BLOCKS \\')
    print('    uint blk_n = 0; \\')
    for k, t in enumerate(traces):
        print('    %sif (cpu.pc == %s) { \\' % ('[branch] ' if k == 0 else '} else ', u(t.start)))
        print('        do { \\')
        for line in t.out:
            print('            %s \\' % line)
        print('        } while (false); \\')
    print('    }')


if __name__ == '__main__':
    main()
