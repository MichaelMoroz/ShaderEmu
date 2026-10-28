"""auipc and what reads it next, from RVC_PROF pcs.txt: GOT loads (lw), addresses (addi), calls (jalr). docs/plans/riscv-dbt.md.

    python tools/rv_compare/dbt_got.py DIR:CLASS [...]
"""
import sys, os
from dbt_model import fields, kind
for arg in sys.argv[1:]:
    path, cls = arg.rsplit(':', 1); cls = int(cls)
    rows = {}
    for line in open(os.path.join(path, 'pcs.txt')):
        if line.startswith('#'): continue
        p = line.split()
        if int(p[0]) != cls: continue
        pc = int(p[2], 16); rows[pc] = (int(p[3], 16), rows.get(pc, (0, 0))[1] + int(p[4]))
    N = sum(e for w, e in rows.values())
    a_lw = a_add = a_jalr = a_other = 0; lwuse = 0
    for pc, (w, e) in rows.items():
        if kind(w) != 'auipc': continue
        rd = fields(w)[1]
        if pc + 4 not in rows: a_other += e; continue
        b, eb = rows[pc + 4]
        ob, rdb, f3b, rs1b, _, _ = fields(b)
        if rs1b != rd: a_other += e; continue
        if ob == 0x03 and f3b == 2:
            a_lw += e
        elif ob == 0x13: a_add += e
        elif ob == 0x67: a_jalr += e
        else: a_other += e
    print('%-22s auipc %.2f%%: +lw (GOT) %.2f  +addi %.2f  +jalr %.2f  other %.2f' % (os.path.basename(path) + (':w' if cls else ''),
          100 * (a_lw + a_add + a_jalr + a_other) / N, 100 * a_lw / N, 100 * a_add / N, 100 * a_jalr / N, 100 * a_other / N))
