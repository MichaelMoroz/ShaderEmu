#!/usr/bin/env python3
"""The lean tick with wide3.h's bundle loop in front of its own fast loop: python make_w3_lean.py DIR [--shape 1001] (DIR's name begins rvc_lean).
wide3.h is written in the names of the tick that is gone: they are turned into lean's here, so that one file says what a bundle does."""
import argparse, os, re, shutil
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, '..', '..'))
ap = argparse.ArgumentParser(); ap.add_argument('dir'); ap.add_argument('--shape', default='1001'); ap.add_argument('--src', default=os.path.join(ROOT, 'experiments', 'rvc_lean'))
a = ap.parse_args()
if os.path.exists(a.dir): shutil.rmtree(a.dir)
shutil.copytree(a.src, a.dir)
w = open(os.path.join(HERE, 'wide3.h'), encoding='utf-8').read().replace('\r\n', '\n')
def must(old, new):
    global w
    assert old in w, old
    w = w.replace(old, new)
must('uint wide_run_l1(L1P uint room) {', 'uint wide_run(inout uint4 c[C_N], uint room) {')
must('    pre_valid = false;\n    fast_same = false;\n    if (!irq_quiet) return 0;\n', '    pre_valid = false;\n')
w = w.replace('cpu.pc', 'pc').replace('cpu.clock', 'clock').replace('fast_budget', 'budget').replace('idx_uint4', 'idx')
w = re.sub(r'mem_set_ram\(([^,]+), (.+?), (\w+), false\);', r'ram_store(c, \1, \2, \3);', w)
w = w.replace('mem_get_cached_or_tex(', 'ram_word(c, ')
w = w.replace('33u', '32u')   # (the float registers lie at 32 here, the sink at 64: the code's numbers are turned by XWR)
must('#define XWR(i, v) xr[i] = (v)\n', '''#if W3_SHAPE & 1024
uint w3_dst(uint i) { return i < 32u ? i : i == 32u ? 64u : i - 1u; }
#define XWR(i, v) xr[w3_dst(i)] = (v)
#else
#define XWR(i, v) xr[(i) << ((i) >> 5)] = (v)
#endif
''')
head = f'#define AS_SIGNED(x) asint(x)\n#define AS_UNSIGNED(x) asuint(x)\n#define W3_SHAPE {a.shape}\n'
p = os.path.join(a.dir, 'tick.hlsl')
s = open(p, encoding='utf-8', newline='').read()
crlf = '\r\n' in s
s = s.replace('\r\n', '\n')
def rep(old, new):
    global s
    assert s.count(old) == 1, old
    s = s.replace(old, new)
rep('// ---- The general step: one instruction of any kind, then the traps and interrupts.\n', head + w + '\n// ---- The general step: one instruction of any kind, then the traps and interrupts.\n')
rep("""                    [branch] if (quiet) n += fast(c, most - n);
                    if (n >= most || stall != 0) break;
                    step(c);
                    n++;
""", """                    uint f_ran = 0;
                    [branch] if (quiet) {
                        n += wide_run(c, most - n);
                        [branch] if (n < most && stall == 0) {
                            f_ran = fast(c, min(most - n, 24u));
                            n += f_ran;
                        }
                    }
                    if (n >= most || stall != 0) break;
                    if (f_ran == 24u) continue;
                    step(c);
                    n++;
""")
open(p, 'w', encoding='utf-8', newline='').write(s.replace('\n', '\r\n') if crlf else s)
print('wrote', a.dir)
