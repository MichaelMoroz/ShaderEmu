#!/usr/bin/env python3
"""The wide tick: a copy of experiments/rvc_opt whose pass runs wide.h's loop first and its RISC-V loop where nothing is translated.
    python make_wide.py DIR [--src experiments/rvc_opt]"""
import argparse, os, shutil, sys

ap = argparse.ArgumentParser()
ap.add_argument('dir')
ap.add_argument('--src', default='experiments/rvc_opt')
ap.add_argument('--count', default='0')
ap.add_argument('--shape', default='0')
a = ap.parse_args()
if os.path.exists(a.dir):
    shutil.rmtree(a.dir)
shutil.copytree(a.src, a.dir)
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wide.h'), os.path.join(a.dir, 'src', 'wide.h'))


def sub(path, old, new):
    p = os.path.join(a.dir, path)
    s = open(p, encoding='utf-8', newline='').read()
    crlf = '\r\n' in s
    s = s.replace('\r\n', '\n')
    if s.count(old) != 1:
        sys.exit(f'{path}: {old!r} found {s.count(old)} times')
    s = s.replace(old, new)
    open(p, 'w', encoding='utf-8', newline='').write(s.replace('\n', '\r\n') if crlf else s)


# after the fast step (its names: fast_exec, xr, fast_budget)
sub('src/cpu.h', 'void cpu_tick_l1(L1P0) {', f'#define WIDE_COUNT {a.count}\n#define WIDE_SHAPE {a.shape}\n#include "wide.h"\n\nvoid cpu_tick_l1(L1P0) {{')
sub('main.shader', """                        i += fast_run(_Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            i++;
                        }
""", """                        i += wide_run_l1(L1A _Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            // (where nothing is translated a short stretch by the old loop, then the map again)
                            uint f_ran = fast_run(min(_Ticks - i, 24u));
                            i += f_ran;
                            if (i < _Ticks && !cpu.stall && (f_ran == 0 || pre_valid)) {
                                cpu_tick_l1(L1A0);
                                i++;
                            }
                        }
""")
print('wrote', a.dir)
