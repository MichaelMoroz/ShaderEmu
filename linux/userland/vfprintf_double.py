#!/usr/bin/env python3
"""vfprintf_double.py MUSL_VFPRINTF_C OUT_C: musl's vfprintf.c with a double's %e, %f, %g and %a made in doubles.

musl formats every floating argument as a long double, which on RV32 is 128 bits wide and all software (__multf3,
__subtf3 and the rest from libgcc's soft-fp, hundreds of instructions an operation). A double's digits come out of
the same algorithm run on doubles: musl's fmt_fp is written for a long double of any width, and with one of 53 bits
(as on 32-bit ARM, where long double is double) its steps are still exact. The copy here is musl's fmt_fp with
long double, LDBL_ and frexpl made double, DBL_ and frexp, called for an argument read as a double (state DBL);
%Le and the rest stay as they were. Checked against the host's printf over random doubles and formats
(linux/userland/test/printf_check.c).
"""
import re
import sys

src = open(sys.argv[1]).read()
start = src.index('static int fmt_fp(FILE *f, long double y, int w, int p, int fl, int t)')
end = src.index('\n}\n', start) + 3
body = src[start:end]
double = body.replace('static int fmt_fp(', 'static int fmt_fp_d(')
double = double.replace('long double', 'double').replace('LDBL_', 'DBL_').replace('frexpl(', 'frexp(')
assert 'long double' not in double and 'LDBL' not in double
src = src[:end] + '\n/* (a double formatted in doubles: vfprintf_double.py) */\n' + double + src[end:]
call = '\t\t\tl = fmt_fp(f, arg.f, w, p, fl, t);\n'
assert src.count(call) == 1, 'musl: the call of fmt_fp moved'
src = src.replace(call, '\t\t\tl = st == LDBL ? fmt_fp(f, arg.f, w, p, fl, t) : fmt_fp_d(f, (double)arg.f, w, p, fl, t);\n')
open(sys.argv[2], 'w').write(src)
