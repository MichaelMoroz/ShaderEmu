"""What other instructions would take away from the code a profile ran (docs/plans/riscv-isa.md): an upper bound read
from RVC_PROF's pcs.txt (tools/rv_compare/prof_parts.py), each executed word with its neighbours as they lie.

    python tools/rv_compare/isa_model.py DIR [DIR ...] [--csv FILE]     (DIR: a part's .prof folder)

A pair of instructions next to each other runs min(count of each) times together: the model assumes the second
follows the first, which holds inside a block and over-counts at a block's head. A lesson of another machine's ISA work:
a model of packing overrated the change 3-4x; programs built with the extension are the measure, this is the bound
and the reason to build them. Counted, by class (0 core 0, 1 the workers):

  zba      slli rd,rs,1..3 then add/sh using rd: sh1add/sh2add/sh3add (one instruction fewer)
  zbb      sext.b/.h (slli 24/16 + srai), zext.h (slli 16 + srli 16), andn/orn (xori -1 then and/or), rotates
           (sll + srl + or of one value), a branch over one mv that min/max makes
  zbs      li 1 + sll + or/andn/xor (bset/bclr/binv), srl + andi 1 (bext)
  zicond   a branch forward over one integer instruction (czero.eqz/nez and an or, about one fewer)
  fmadd    fmul then fadd/fsub (or fneg) reading its result next: one fewer
  fmv      fmv.x.w and fmv.w.x (gone under Zfinx), flw/fsw
  pairs    two word stores (loads) next to each other from one base, the offsets 4 apart and the lower on 8: a pair
  general  what the tick's fast step does not make (the general path: CSRs, fences, fmadd family, the rest)
"""
import argparse, collections, os, sys


def kind(w):
    op = w & 0x7f
    return op


def fields(w):
    return (w & 0x7f, (w >> 7) & 31, (w >> 12) & 7, (w >> 15) & 31, (w >> 20) & 31, w >> 25)


def imm_i(w):
    return ((w & 0xffffffff) >> 20) - (4096 if w & 0x80000000 else 0)


def imm_s(w):
    v = ((w >> 25) << 5) | ((w >> 7) & 31)
    return v - 4096 if v & 0x800 else v


def reads(w):
    op, rd, f3, rs1, rs2, f7 = fields(w)
    if op in (0x33, 0x23, 0x63, 0x2f):
        return {rs1, rs2}
    if op in (0x13, 0x03, 0x67, 0x07, 0x27):
        return {rs1} | ({rs2} if op == 0x27 else set())
    return set()


def is_fast(w):
    """the tick's fast step (experiments/rvc_opt/src/emu.h fast_exec_l1) makes it"""
    op, rd, f3, rs1, rs2, f7 = fields(w)
    if op in (0x13, 0x33):
        if op == 0x33 and f7 == 1:
            return True
        exist = 0xffff if f7 == 0 else 0x21fd if f7 == 0x20 else 0x00dd
        return bool((exist >> (((w >> 2) & 8) | f3)) & 1)
    if op in (0x03, 0x23, 0x63, 0x37, 0x17, 0x6f, 0x67, 0x07, 0x27, 0x53):
        return True
    if op == 0x2f:
        return f3 == 2 and (w >> 27) in (0, 1, 2, 3, 4, 8, 12)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--csv')
    a = ap.parse_args()
    rows = []
    for d in a.dirs:
        name = os.path.basename(d.rstrip('/\\')).replace('.prof', '')
        code = collections.defaultdict(dict)   # (class, table) -> pc -> (word, exec, stores, newtex, loads)
        for line in open(os.path.join(d, 'pcs.txt')):
            if line.startswith('#'):
                continue
            p = line.split()
            cls, table, pc, word, ex, ld, st, nt = int(p[0]), p[1], int(p[2], 16), int(p[3], 16), int(p[4]), int(p[5]), int(p[6]), int(p[7])
            e = code[(cls, table)].get(pc)
            if e:   # (several harts' entries of one pc)
                code[(cls, table)][pc] = (word, e[1] + ex, e[2] + st, e[3] + nt, e[4] + ld)
            else:
                code[(cls, table)][pc] = (word, ex, st, nt, ld)
        for cls in (0, 1):
            C = collections.Counter()
            for (c, table), pcs in code.items():
                if c != cls:
                    continue
                for pc, (w, ex, st, nt, ld) in pcs.items():
                    C['all'] += ex
                    C['stores'] += st
                    C['newtex'] += nt
                    op, rd, f3, rs1, rs2, f7 = fields(w)
                    if not is_fast(w):
                        C['general'] += ex
                        if op in (0x43, 0x47, 0x4b, 0x4f):
                            C['general fmadd'] += ex
                        elif op == 0x73:
                            C['general csr/system'] += ex
                    if op == 0x53 and f7 in (0x70, 0x78) and (f7 == 0x78 or f3 == 0):
                        C['fmv'] += ex
                    if op in (0x07, 0x27):
                        C['flw/fsw'] += ex
                    if op == 0x53:
                        C['float op'] += ex
                    if op == 0x23:
                        C['store ins'] += ex
                    if op == 0x03:
                        C['load ins'] += ex
                    nx = pcs.get(pc + 4)
                    if not nx:
                        continue
                    w2, ex2 = nx[0], nx[1]
                    both = min(ex, ex2)
                    if both == 0:
                        continue
                    o2, rd2, f32, r12, r22, f72 = fields(w2)
                    # Zba: slli by 1-3 then an add reading it
                    if op == 0x13 and f3 == 1 and f7 == 0 and 1 <= rs2 <= 3 and o2 == 0x33 and f72 == 0 and f32 == 0 and rd in (r12, r22):
                        C['zba'] += both
                    # Zbb: sext.b/.h, zext.h
                    if op == 0x13 and f3 == 1 and f7 == 0 and rs2 in (16, 24) and o2 == 0x13 and f32 == 5 and r12 == rd and r22 == rs2:
                        C['zbb'] += both
                    # andn/orn/xnor: xori -1 then and/or/xor reading it
                    if op == 0x13 and f3 == 4 and imm_i(w) == -1 and o2 == 0x33 and f72 == 0 and f32 in (4, 6, 7) and rd in (r12, r22):
                        C['zbb'] += both
                    # min/max: a branch over a mv (addi rd, rs, 0)
                    if op == 0x63 and (w & 0x80000f80) == 0x400 and ((w >> 25) & 0x3f) == 0 and o2 == 0x13 and f32 == 0 and imm_i(w2) == 0 and rd2 in (rs1, rs2) and r12 in (rs1, rs2):
                        C['zbb minmax'] += both
                    # Zbs: srl then andi 1
                    if op == 0x33 and f3 == 5 and f7 == 0 and o2 == 0x13 and f32 == 7 and imm_i(w2) == 1 and r12 == rd:
                        C['zbs'] += both
                    # Zicond: a branch forward over one integer instruction
                    if op == 0x63 and (w & 0x80000f80) == 0x400 and ((w >> 25) & 0x3f) == 0 and o2 in (0x13, 0x33) and not (o2 == 0x33 and f72 == 1):
                        C['zicond'] += both
                    # fmadd: fmul.s then fadd/fsub reading its result
                    if op == 0x53 and f7 == 0x08 and o2 == 0x53 and f72 in (0x00, 0x04) and rd in (r12, r22):
                        C['fmadd'] += both
                    # custom-shaped: a constant made for the branch after it (li then a branch reading it)
                    if op == 0x13 and f3 == 0 and rs1 == 0 and o2 == 0x63 and rd in (r12, r22) and rd != 0:
                        C['li+branch'] += both
                    # lui then addi/lw/sw on it (a 32-bit constant or address): one with a long immediate
                    if op == 0x37 and ((o2 == 0x13 and f32 == 0) or o2 in (0x03, 0x23, 0x07, 0x27)) and r12 == rd:
                        C['lui+use'] += both
                    # auipc then addi/lw/jalr on it (PC-relative address, a GOT load, a far call)
                    if op == 0x17 and (o2 in (0x13, 0x03, 0x23, 0x67, 0x07, 0x27)) and r12 == rd:
                        C['auipc+use'] += both
                    # an add then a load or store using it as the base: an indexed access
                    if op == 0x33 and f3 == 0 and f7 in (0, 0x10) and o2 in (0x03, 0x23, 0x07, 0x27) and r12 == rd:
                        C['add+mem'] += both
                    # addi then a branch reading the same register (a loop's counter: a fused add-and-branch)
                    if op == 0x13 and f3 == 0 and rs1 == rd and rd != 0 and o2 == 0x63 and rd in (r12, r22):
                        C['addi+branch'] += both
                    # pairs: two word stores (loads) from one base, offsets 4 apart, the lower on 8
                    if op == 0x23 and f3 == 2 and o2 == 0x23 and f32 == 2 and rs1 == r12:
                        lo = min(imm_s(w), imm_s(w2))
                        if abs(imm_s(w) - imm_s(w2)) == 4 and lo % 8 == 0:
                            C['store pairs'] += both
                    if op == 0x03 and f3 == 2 and o2 == 0x03 and f32 == 2 and rs1 == r12 and rd != rs1:
                        lo = min(imm_i(w), imm_i(w2))
                        if abs(imm_i(w) - imm_i(w2)) == 4 and lo % 8 == 0:
                            C['load pairs'] += both
            if C['all'] == 0:
                continue
            rows.append((name, cls, C))
    keys = ['all', 'stores', 'newtex', 'general', 'general fmadd', 'general csr/system', 'zba', 'zbb', 'zbb minmax', 'zbs', 'zicond',
            'fmadd', 'fmv', 'flw/fsw', 'float op', 'store ins', 'store pairs', 'load ins', 'load pairs',
            'li+branch', 'lui+use', 'auipc+use', 'add+mem', 'addi+branch']
    out = open(a.csv, 'w') if a.csv else None
    hdr = 'part,class,' + ','.join(keys)
    print(hdr)
    if out:
        out.write(hdr + '\n')
    for name, cls, C in rows:
        line = f'{name},{cls},' + ','.join(str(C[k]) for k in keys)
        print(line)
        if out:
            out.write(line + '\n')


if __name__ == '__main__':
    sys.exit(main())
