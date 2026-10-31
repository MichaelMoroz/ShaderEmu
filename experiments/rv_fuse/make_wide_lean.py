#!/usr/bin/env python3
"""The world's tick with wide_lean.h's loop first: python make_wide_lean.py DIR (a folder whose name begins rvc_lean)."""
import os, shutil, sys
d = sys.argv[1]
if os.path.exists(d):
    shutil.rmtree(d)
shutil.copytree('experiments/rvc_lean', d)
p = os.path.join(d, 'tick.hlsl')
s = open(p, encoding='utf-8', newline='').read()
crlf = '\r\n' in s
s = s.replace('\r\n', '\n')


def rep(a, b):
    global s
    assert s.count(a) == 1, a
    s = s.replace(a, b)


rep("// ---- The general step: one instruction of any kind, then the traps and interrupts.\n",
    open('experiments/rv_fuse/wide_lean.h', encoding='utf-8').read() + "\n// ---- The general step: one instruction of any kind, then the traps and interrupts.\n")
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
print('wrote', d)
