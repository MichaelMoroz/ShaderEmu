#!/usr/bin/env python3
"""A tick with wide3.h's loop: python make_w3.py DIR [--shape N] [--src experiments/rvc_opt]"""
import argparse, os, shutil, sys
ap = argparse.ArgumentParser()
ap.add_argument('dir'); ap.add_argument('--src', default='experiments/rvc_opt'); ap.add_argument('--shape', default='0'); ap.add_argument('--no-old', action='store_true', help='the old loop out of the shader: what is left at goes by the general step'); ap.add_argument('--no-fp', action='store_true', help='the floats out of the general step (no good for a load with floats)')
a = ap.parse_args()
if os.path.exists(a.dir): shutil.rmtree(a.dir)
shutil.copytree(a.src, a.dir)
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wide3.h'), os.path.join(a.dir, 'src', 'wide3.h'))
def sub(path, old, new):
    p = os.path.join(a.dir, path)
    s = open(p, encoding='utf-8', newline='').read()
    crlf = '\r\n' in s
    s = s.replace('\r\n', '\n')
    if s.count(old) != 1: sys.exit(f'{path}: {old!r} found {s.count(old)} times')
    s = s.replace(old, new)
    open(p, 'w', encoding='utf-8', newline='').write(s.replace('\n', '\r\n') if crlf else s)
sub('src/cpu.h', 'void cpu_tick_l1(L1P0) {', f'#define W3_SHAPE {a.shape}\n#include "wide3.h"\n#define wide_run(a0) wide_run_l1(L1A a0)\n\nvoid cpu_tick_l1(L1P0) {{')
if a.no_old:
    new_body = """                        i += wide_run_l1(L1A _Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            i++;
                        }
"""
else:
    new_body = """                        i += wide_run_l1(L1A _Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            uint f_ran = fast_run(min(_Ticks - i, 24u));
                            i += f_ran;
                            if (i < _Ticks && !cpu.stall && (f_ran == 0 || pre_valid)) {
                                cpu_tick_l1(L1A0);
                                i++;
                            }
                        }
"""
sub('main.shader', """                        i += fast_run(_Ticks - i);
                        if (i < _Ticks && !cpu.stall) {
                            cpu_tick_l1(L1A0);
                            i++;
                        }
""", new_body)

if a.no_fp:
    sub('src/emu.h', "val = fp_exec(w, fp_a, fp_b, to_f, ok);", "ok = false;")
    sub('src/emu.h', "uint fp_val = fp_exec(ins_word, xr[ins_FormatR.rs1 + (fp_int_source ? 0 : XR_F)], xr[XR_F + ins_FormatR.rs2], fp_to_float, fp_ok);", "uint fp_val = 0; fp_ok = false;")
print('wrote', a.dir)
