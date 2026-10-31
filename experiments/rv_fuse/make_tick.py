#!/usr/bin/env python3
"""The fused tick: experiments/rvc_opt with a word whose bit 0 is clear standing for its RISC-V instruction plus the next
word, a plain instruction of a kind FUSE_SET says, on one trip of the fast loop.

    python make_tick.py DIR [--set SECOND] [--from experiments/rvc_opt]
        SECOND: addi (default), arith (addi, add, sub), ...

The patch is kept as a script because it lost: DIR is a copy of the tree's tick with these edits. RTX 5070 Laptop, D3D11,
fxc2, 31 October 2026, from one snapshot each (snaps.ps1, fuse_snap.py), in turn with the tree, millions of instructions a
second tree -> fused: --shape inkind --set addi (11% merged on the shell): shell 5.25 -> 5.38, Doom 4.91 -> 4.92, Quake 8.15
-> 8.10, nxray 627 -> 624. --set addbr (22, 19, 17%): shell 5.21 -> 5.34, Doom -> 5.05, Quake -> 8.34. --straddle --fp --fp2
(27, 24, 25, 27%): shell 5.21 -> 5.16 to 5.21, Doom -> 4.95, nxray 630 -> 626. On plain code (RVC_TICK_ALT): inkind addi +1.3%,
addbr +2.6%, the last +5.3%, --shape noret +10%, ret +12%. A merged instruction saves 25 to 65 ns of a mean 175.
"""
import argparse, os, re, shutil, sys

ap = argparse.ArgumentParser()
ap.add_argument('dir')
ap.add_argument('--set', default='addi')
ap.add_argument('--src', default='experiments/rvc_opt')
ap.add_argument('--shape', default='noret')
ap.add_argument('--first', default='alu,load,store', help='shape inkind: the kinds that may be a first op')
ap.add_argument('--straddle', action='store_true', help='a pair may lie across two texels')
ap.add_argument('--fp', action='store_true', help='float kinds may be a first op')
ap.add_argument('--fp2', action='store_true', help='fadd, fsub, fmul may be a second op')
ap.add_argument('--mem', action='store_true', help='a load may take a second word of its texel, a store likewise')
ap.add_argument('--edits', default='mask,n,gen', help='which of the fused tick edits are made')
a = ap.parse_args()
if os.path.exists(a.dir):
    shutil.rmtree(a.dir)
shutil.copytree(a.src, a.dir)


E = set(a.edits.split(','))
TAG = 'mask'


def sub(path, old, new, count=1):
    if TAG not in E: return
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

TAG = 'second'
E.add('second')
SECONDS = {
    # addi, add, sub, or a branch (bit 6): only a fused word passes this test
    'addbr': """
        uint f2 = fw_cur.y;
        [branch]
        if ((f2 & 0x40) != 0) {
FP2_PREFIX
            uint f2a = xreg((f2 >> 15) & 0x1f), f2c = xreg((f2 >> 20) & 0x1f);
            uint f2f = (f2 >> 12) & 7;
            bool f2lt = (f2f & 2) ? f2a < f2c : AS_SIGNED(f2a) < AS_SIGNED(f2c);
            bool f2t = ((f2f & 4) ? f2lt : f2a == f2c) != ((f2f & 1) != 0);
            FormatB f2br = parse_FormatB(f2);
            if (f2t) next2 = pc + 4 + f2br.imm;
FP2_SUFFIX
        } else {
            uint f2b = (f2 & 0x20) != 0 ? ((f2 & 0x40000000) != 0 ? 0u - xreg((f2 >> 20) & 0x1f) : xreg((f2 >> 20) & 0x1f)) : AS_UNSIGNED(AS_SIGNED(f2) >> 20);
            xr[(f2 >> 7) & 0x1f] = xreg((f2 >> 15) & 0x1f) + f2b;
        }
""",
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

FP2P = """            [branch]
            if ((f2 & 0x20) == 0) {
                uint f27 = f2 >> 25;
                precise float f2x = asfloat(xr[XR_F + ((f2 >> 15) & 0x1f)]);
                precise float f2y = asfloat(xr[XR_F + ((f2 >> 20) & 0x1f)]);
                precise float f2s = f2x + f2y;
                precise float f2d = f2x - f2y;
                precise float f2m = f2x * f2y;
                uint f2v = asuint(f27 == 0x00 ? f2s : f27 == 0x04 ? f2d : f2m);
                if ((f2v & 0x7fffffff) > 0x7f800000) f2v = 0x7fc00000;
                xr[XR_F + ((f2 >> 7) & 0x1f)] = f2v;
            } else {
"""
SECONDS['addbr'] = SECONDS['addbr'].replace('FP2_PREFIX\n', FP2P if a.fp2 else '').replace('FP2_SUFFIX\n', '            }\n' if a.fp2 else '')
SEQ = '((pc & 12) == 12 ? 0u : pc + 8)' if a.straddle else 'pc + 8'
for k in SECONDS:
    if a.straddle:
        SECONDS[k] = SECONDS[k].replace('uint f2 = fw_cur.y;', 'uint f2 = (pc & 12) == 12 ? fw_tex1.x : fw_cur.y;')
if a.shape == 'ret':
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
elif a.shape == 'noret':
    sub('emu.h', """    cpu.pc = npc;
    // (the texel's next word is the next instruction; a jump within the texel is fetched anew)
    fast_same = npc == pc + 4 && (npc & 12) != 0;
    fw_cur = fw_cur.yzwx;
""", """    fast_same = npc == pc + 4 && (npc & 12) != 0;
    [branch]
    if ((w & 1) == 0) {""" + SECONDS[a.set] + """        fast_n++;
        npc = pc + 8;
        fast_same = (npc & 12) != 0;
        fw_cur = fw_cur.yzwx;
    }
    cpu.pc = npc;
    fw_cur = fw_cur.yzwx;
""")
elif a.shape == 'inkind':
    FIRST = set(a.first.split(','))
    TAIL = """
                [branch]
                if ((w & 1) == 0) {
                    xr[rd] = val;
                    wr = false;
                    uint next2 = pc + 8;""" + SECONDS[a.set] + """                    fast_n++;
                    npc = next2;
                    seq = """ + SEQ + """;
                    fw_cur = fw_cur.yzwx;
                }
"""
    STORE_TAIL = """
                    [branch]
                    if ((w & 1) == 0) {
                        uint next2 = pc + 8;""" + SECONDS[a.set] + """                        fast_n++;
                        npc = next2;
                        seq = """ + SEQ + """;
                        fw_cur = fw_cur.yzwx;
                    }
"""
    sub('emu.h', "    uint npc = pc + 4;\n    uint val = 0;\n    bool wr = true;\n    bool ok = true;\n    // A switch is", "    uint npc = pc + 4;\n    uint seq = npc;\n    uint val = 0;\n    bool wr = true;\n    bool ok = true;\n    // A switch is")
    sub('emu.h', "    fast_same = npc == pc + 4 && (npc & 12) != 0;", "    fast_same = npc == seq && (npc & 12) != 0;")
    if a.mem:
        LOAD_TAIL = """
                    [branch]
                    if ((w & 1) == 0) {
                        xr[rd] = val;
                        wr = false;
                        uint f2 = fw_cur.y;
                        bool took = true;
                        [branch]
                        if ((f2 & 0x10) == 0) {
                            uint a2 = xreg((f2 >> 15) & 0x1f) + AS_UNSIGNED(AS_SIGNED(f2) >> 20);
                            took = (a2 >> 4) == ((pa & 0x7fffffff) >> 4) && (a2 & 3) == 0;
                            xr[took ? ((f2 >> 7) & 0x1f) : 32] = idx_uint4(mem_tx, (a2 >> 2) & 3);
                        } else {
                            xr[(f2 >> 7) & 0x1f] = xreg((f2 >> 15) & 0x1f) + AS_UNSIGNED(AS_SIGNED(f2) >> 20);
                        }
                        [branch]
                        if (took) {
                            fast_n++;
                            npc = pc + 8;
                            seq = npc;
                            fw_cur = fw_cur.yzwx;
                        }
                    }
"""
        STORE_TAIL = """
                    [branch]
                    if ((w & 1) == 0) {
                        uint f2 = fw_cur.y;
                        bool took = true;
                        [branch]
                        if ((f2 & 0x10) == 0) {
                            uint a2 = xreg((f2 >> 15) & 0x1f) + ((AS_UNSIGNED(AS_SIGNED(f2) >> 20) & 0xffffffe0) | ((f2 >> 7) & 0x1f));
                            uint t2 = (a2 & 0x7fffffff) >> 4;
                            took = t2 == ((pa & 0x7fffffff) >> 4) && (a2 & 3) == 0 && t2 == l1_last_t;
                            [branch]
                            if (took) {
                                uint b2 = xreg((f2 >> 20) & 0x1f);
                                uint k2 = (a2 >> 2) & 3;
                                mem_cache_bloom |= a2 & 0x7ffffffc;
                                [branch]
                                if (k2 < 2) {
                                    if (k2 == 0) { L1_DATA(l1_last_at).x = b2; } else { L1_DATA(l1_last_at).y = b2; }
                                } else {
                                    if (k2 == 2) { L1_DATA(l1_last_at).z = b2; } else { L1_DATA(l1_last_at).w = b2; }
                                }
                            }
                        } else {
                            xr[(f2 >> 7) & 0x1f] = xreg((f2 >> 15) & 0x1f) + AS_UNSIGNED(AS_SIGNED(f2) >> 20);
                        }
                        [branch]
                        if (took) {
                            fast_n++;
                            npc = pc + 8;
                            seq = npc;
                            fw_cur = fw_cur.yzwx;
                        }
                    }
"""
        TAILL = LOAD_TAIL
    else:
        TAILL = TAIL
    if 'alu' in FIRST:
        sub('emu.h', "                ok = ((exist >> (((w >> 2) & 8) | f3)) & 1) != 0;\n", "                ok = ((exist >> (((w >> 2) & 8) | f3)) & 1) != 0;\n" + TAIL)
    if 'load' in FIRST:
        sub('emu.h', "                    val = (f3 & 4) ? v >> up : AS_UNSIGNED(AS_SIGNED(v) >> up);\n                    ok = true;\n", "                    val = (f3 & 4) ? v >> up : AS_UNSIGNED(AS_SIGNED(v) >> up);\n                    ok = true;\n" + TAILL)
    if a.fp:
        sub('emu.h', "                xr[(ok && to_f) ? XR_F + ((w >> 7) & 0x1f) : 32] = val;   // (or to the scratch slot)\n",
            "                xr[(ok && to_f) ? XR_F + ((w >> 7) & 0x1f) : 32] = val;   // (or to the scratch slot)\n" + STORE_TAIL.replace('if ((w & 1) == 0) {', 'if ((w & 1) == 0 && ok) {'))
    if 'store' in FIRST:
        sub('emu.h', "                    mem_set_ram(pa & 0x7ffffffc, rs2v << (s_off * 8), s_mask, false);\n                    ok = true;\n", "                    mem_set_ram(pa & 0x7ffffffc, rs2v << (s_off * 8), s_mask, false);\n                    ok = true;\n" + STORE_TAIL)
else:
    assert a.shape == 'none'
if a.mem:
    sub('emu.h', 'static bool fast_same = false;', 'static uint4 mem_tx = 0;'+chr(10)+'static bool fast_same = false;')
    sub('mem.h', '    uint v = idx_uint4(tx, (addr >> 2) & 0x3);'+chr(10)+'    // (the word the cache', '    mem_tx = tx;'+chr(10)+'    uint v = idx_uint4(tx, (addr >> 2) & 0x3);'+chr(10)+'    // (the word the cache')
TAG = 'n'
# one count of a run's instructions, which a fused word adds to
sub('emu.h', 'static bool fast_same = false;', 'static uint fast_n = 0;\nstatic bool fast_same = false;')
sub('cpu.h', '    uint n = 0;\n    [branch]\n    if (fast_budget > 0) {', '    fast_n = 0;\n    [branch]\n    if (fast_budget > 0) {')
sub('cpu.h', '            n++;\n            // (a store that stalled has made the budget none: types.h)\n            if (n >= fast_budget) {', '            fast_n++;\n            // (a store that stalled has made the budget none: types.h)\n            if (fast_n >= fast_budget) {')
sub('cpu.h', '    if (n > 0 && cpu.stall) {', '    if (fast_n > 0 && cpu.stall) {')
sub('cpu.h', '    cpu.clock += n;', '    cpu.clock += fast_n;')
sub('cpu.h', '    return n;\n}', '    return fast_n;\n}')
TAG = 'gen'
# the general path takes the first of a fused word's two by itself: the next word is an instruction as it was
sub('emu.h', '            if (pre_valid) { ins_word = pre_word; } else { ins_word = mem_get_instruction(ins_addr); }\n',
    '            if (pre_valid) { ins_word = pre_word; } else { ins_word = mem_get_instruction(ins_addr); }\n            if ((ins_word & 3) == 2) ins_word |= 1;\n')
print('wrote', a.dir, 'second ops:', a.set)
