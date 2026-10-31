#!/usr/bin/env python3
"""The fused tick: experiments/rvc_opt with a word whose bit 0 is clear standing for its RISC-V instruction plus the next
word, a plain instruction of a kind FUSE_SET says, on one trip of the fast loop.

    python make_tick.py DIR [--set SECOND] [--from experiments/rvc_opt]
        SECOND: addi (default), arith (addi, add, sub), ...

The patch is kept as a script while it is an experiment: DIR is a copy of the tree's tick with these edits.
"""
import argparse, os, re, shutil, sys

ap = argparse.ArgumentParser()
ap.add_argument('dir')
ap.add_argument('--set', default='addi')
ap.add_argument('--src', default='experiments/rvc_opt')
a = ap.parse_args()
if os.path.exists(a.dir):
    shutil.rmtree(a.dir)
shutil.copytree(a.src, a.dir)


def sub(path, old, new, count=1):
    p = os.path.join(a.dir, 'src', path)
    s = open(p, encoding='utf-8', newline='').read()
    if s.count(old) < 1 or (count and s.count(old) != count):
        sys.exit(f'{path}: {old!r} found {s.count(old)} times')
    s = s.replace(old, new)
    open(p, 'w', encoding='utf-8', newline='').write(s)


# the kinds' tests leave out bit 0: a fused word is otherwise its instruction
sub('emu.h', '#define K_ALU ((w & 0x5f) == 0x13 && (w & 0xfe00007f) != 0x02000033)', '#define K_ALU ((w & 0x5e) == 0x12 && (w & 0xfe00007e) != 0x02000032)')
sub('emu.h', '#define K_FP ((w & 0x5f) == 0x07 || opc == 0x53)', '#define K_FP ((w & 0x5e) == 0x06 || opc == 0x52)')
sub('emu.h', '    uint opc = w & 0x7f;\n    uint f3 = (w >> 12) & 0x7;\n    uint rd = (w >> 7) & 0x1f;\n    // rs1 and rs2 sit', '    uint opc = w & 0x7e;\n    uint f3 = (w >> 12) & 0x7;\n    uint rd = (w >> 7) & 0x1f;\n    // rs1 and rs2 sit')
sub('emu.h', 'if (K_ALU || opc == 0x03 || opc == 0x23 || K_FP) {', 'if (K_ALU || opc == 0x02 || opc == 0x22 || K_FP) {')
sub('emu.h', 'if (K_ALU || opc == 0x03 || opc == 0x23) {', 'if (K_ALU || opc == 0x02 || opc == 0x22) {')
sub('emu.h', '        if (K_ALU || opc == 0x03) {', '        if (K_ALU || opc == 0x02) {')
sub('emu.h', '            if (opc == 0x23) {', '            if (opc == 0x22) {')
sub('emu.h', '                if (opc == 0x53) {', '                if (opc == 0x52) {')
sub('emu.h', '                } else if (opc == 0x07) {', '                } else if (opc == 0x06) {')
sub('emu.h', '    } else if (opc == 0x63) {                                           // branches', '    } else if (opc == 0x62) {                                           // branches')
sub('emu.h', "(w & 0x5f) != 0x07) {", "(w & 0x5e) != 0x06) {")
sub('emu.h', '    } else if (opc == 0x2f) {', '    } else if (opc == 0x2e) {')
sub('emu.h', '            case 0x33: {                                            // the M extension (f7 == 1)', '            case 0x32: {                                            // the M extension (f7 == 1)')
sub('emu.h', 'ok = (w & 3) == 3 && ((0x3044u', 'ok = (w & 2) == 2 && ((0x3044u')

SECONDS = {
    'addi': """
        uint f2 = fw_cur.y;
        xr[(f2 >> 7) & 0x1f] = xreg((f2 >> 15) & 0x1f) + AS_UNSIGNED(AS_SIGNED(f2) >> 20);
""",
    # addi, add, sub: bit 5 a register, bit 30 of a register's form the difference (rd is never x0)
    'arith': """
        uint f2 = fw_cur.y;
        uint f2b = (f2 & 0x20) != 0 ? ((f2 & 0x40000000) != 0 ? 0u - xreg((f2 >> 20) & 0x1f) : xreg((f2 >> 20) & 0x1f)) : AS_UNSIGNED(AS_SIGNED(f2) >> 20);
        xr[(f2 >> 7) & 0x1f] = xreg((f2 >> 15) & 0x1f) + f2b;
""",
}

sub('emu.h', """    xr[(wr && rd != 0) ? rd : 32] = val;   // branch-free: discarded writes go to the scratch slot
""", """    xr[(wr && rd != 0) ? rd : 32] = val;   // branch-free: discarded writes go to the scratch slot
    [branch]
    if ((w & 1) == 0) {""" + SECONDS[a.set] + """        fast_n++;
        cpu.pc = pc + 8;
        fast_same = ((pc + 8) & 12) != 0;
        fw_cur = fw_cur.zwxy;
        return true;
    }
""")
# one count of a run's instructions, which a fused word adds to
sub('emu.h', 'static bool fast_same = false;', 'static uint fast_n = 0;\nstatic bool fast_same = false;')
sub('cpu.h', '    uint n = 0;\n    [branch]\n    if (fast_budget > 0) {', '    fast_n = 0;\n    [branch]\n    if (fast_budget > 0) {')
sub('cpu.h', '            n++;\n            // (a store that stalled has made the budget none: types.h)\n            if (n >= fast_budget) {', '            fast_n++;\n            // (a store that stalled has made the budget none: types.h)\n            if (fast_n >= fast_budget) {')
sub('cpu.h', '    if (n > 0 && cpu.stall) {', '    if (fast_n > 0 && cpu.stall) {')
sub('cpu.h', '    cpu.clock += n;', '    cpu.clock += fast_n;')
sub('cpu.h', '    return n;\n}', '    return fast_n;\n}')
# the general path takes the first of a fused word's two by itself: the next word is an instruction as it was
sub('emu.h', '            if (pre_valid) { ins_word = pre_word; } else { ins_word = mem_get_instruction(ins_addr); }\n',
    '            if (pre_valid) { ins_word = pre_word; } else { ins_word = mem_get_instruction(ins_addr); }\n            if ((ins_word & 3) == 2) ins_word |= 1;\n')
print('wrote', a.dir, 'second ops:', a.set)
