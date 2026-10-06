"""Downloads the third-party sources the Linux programs are built from, into ../build/fetch.
Nothing fetched is kept in this repository.

  glxgears.c            Mesa demos 8.4.0 (Brian Paul, MIT-style licence); compiled unmodified
  builtins/*            LLVM compiler-rt 18.1.8 (Apache-2.0 WITH LLVM-exception): soft-float and
                        64-bit integer routines, since the CPU is RV32IMA without an FPU
"""
import os, urllib.request

here = os.path.dirname(os.path.abspath(__file__))
dest = os.path.join(here, '..', 'build', 'fetch')
RT = 'https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-18.1.8/compiler-rt/lib/builtins/'
BUILTINS = '''int_lib.h int_types.h int_endianness.h int_util.h int_math.h int_div_impl.inc fp_lib.h fp_mode.h
fp_add_impl.inc fp_mul_impl.inc fp_div_impl.inc fp_compare_impl.inc fp_extend.h fp_extend_impl.inc fp_trunc.h
fp_trunc_impl.inc fp_fixint_impl.inc fp_fixuint_impl.inc int_to_fp.h int_to_fp_impl.inc addsf3.c adddf3.c subsf3.c
subdf3.c mulsf3.c muldf3.c divsf3.c divdf3.c comparesf2.c comparedf2.c extendsfdf2.c truncdfsf2.c fixsfsi.c fixdfsi.c
fixunssfsi.c fixunsdfsi.c floatsisf.c floatsidf.c floatunsisf.c floatunsidf.c negsf2.c negdf2.c udivdi3.c divdi3.c
umoddi3.c moddi3.c udivmoddi4.c muldi3.c ashldi3.c lshrdi3.c ashrdi3.c fixdfdi.c fixunsdfdi.c floatdidf.c floatundidf.c
fp_mode.c'''.split()
FILES = [('glxgears.c', 'https://gitlab.freedesktop.org/mesa/demos/-/raw/mesa-demos-8.4.0/src/xdemos/glxgears.c')]
FILES += [('builtins/' + f, RT + f) for f in BUILTINS]

for name, url in FILES:
    path = os.path.join(dest, name)
    if os.path.exists(path):
        continue
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print('fetching', name)
    with urllib.request.urlopen(url, timeout=30) as r:
        open(path, 'wb').write(r.read())
