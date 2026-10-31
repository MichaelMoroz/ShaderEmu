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
OLD = """                        i += fast_run(_Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            i++;
                        }
"""
if int(a.shape) & 65536:
    # the old loop first in the text (a test of what a loop before it costs it)
    sub('main.shader', OLD, """                        uint f_ran = fast_run(min(_Ticks - i, 24u));
                        i += f_ran;
                        bool f_pre = pre_valid;
                        if (i < _Ticks && !cpu.stall && !f_pre) {
                            i += wide_run_l1(L1A _Ticks - i);
                        }
                        if (i < _Ticks && !cpu.stall && (f_ran == 0 || f_pre)) {
                            pre_valid = f_pre;
                            cpu_tick_l1(L1A0);
                            i++;
                        }
""")
elif int(a.shape) & 32:
    # the old loop is not in the shader: what is not translated goes by the general step, one at a time
    sub('main.shader', OLD, """                        i += wide_run_l1(L1A _Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            i++;
                        }
""")
else:
    sub('main.shader', OLD, """                        i += wide_run_l1(L1A _Ticks - i);
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
if int(a.shape) & (1 << 29):
    # (a test for the shell loop only: the whole shader smaller, is the doubling a limit of its size? the floats' long path gone)
    sub('src/emu.h', "val = fp_exec(w, fp_a, fp_b, to_f, ok);", "ok = false;")
    sub('src/emu.h', "uint fp_val = fp_exec(ins_word, xr[ins_FormatR.rs1 + (fp_int_source ? 0 : XR_F)], xr[XR_F + ins_FormatR.rs2], fp_to_float, fp_ok);", "uint fp_val = 0; fp_ok = false;")
print('wrote', a.dir)
