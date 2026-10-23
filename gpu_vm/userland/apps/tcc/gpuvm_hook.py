"""TinyCC for gpu_vm (gpu_vm/userland/apps/tcc/README.md): puts gpuvm-gen.c, gpuvm-link.c and gpuvm-asm.c into jrrk2's
riscv32 tree and makes TCC_TARGET_GPUVM a target there. Idempotent; after linux/tcc's hooks.

    python3 gpuvm_hook.py TINYCC

TCC_TARGET_GPUVM is a variant of TCC_TARGET_RISCV32 (TinyCC's 32-bit paths that RISC-V's port made: a long long and a
double as two words, RELA): it defines it too, and these edits take gpu_vm's files, machine and relocations where the
RISC-V ones would be.
"""
import os
import shutil
import sys

src = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
for f in ('gpuvm-gen.c', 'gpuvm-link.c', 'gpuvm-asm.c'):
    shutil.copyfile(os.path.join(here, f), os.path.join(src, f))

MARK = 'TCC_TARGET_GPUVM'


def patch(name, edits):
    path = os.path.join(src, name)
    s = open(path).read()
    if MARK in s:
        return
    for old, new in edits:
        assert s.count(old) == 1, (name, old)
        s = s.replace(old, new)
    open(path, 'w').write(s)
    print(name + ': gpu_vm')


INCLUDES = ('''#elif defined(TCC_TARGET_RISCV32)
#include "riscv32-gen.c"
#include "riscv32-link.c"
#include "riscv32-asm.c"''', '''#elif defined(TCC_TARGET_GPUVM)
#include "gpuvm-gen.c"
#include "gpuvm-link.c"
#include "gpuvm-asm.c"
#elif defined(TCC_TARGET_RISCV32)
#include "riscv32-gen.c"
#include "riscv32-link.c"
#include "riscv32-asm.c"''')

patch('tcc.h', [
    ('''/* default target is I386 */''', '''/* gpu_vm: a variant of riscv32's 32-bit target (gpu_vm/userland/apps/tcc) */
#if defined TCC_TARGET_GPUVM && !defined TCC_TARGET_RISCV32
# define TCC_TARGET_RISCV32 1
#endif

/* default target is I386 */'''),
    (INCLUDES[0].replace('#include', '# include'), INCLUDES[1].replace('#include', '# include')),
])

patch('libtcc.c', [
    INCLUDES,
    ('''    s->unwind_tables = 1;''', '''#ifndef TCC_TARGET_GPUVM   /* (its frames are not described) */
    s->unwind_tables = 1;
#endif'''),
    ('''#elif defined TCC_TARGET_RISCV32
    "riscv32"''', '''#elif defined TCC_TARGET_GPUVM
    "gpuvm"
#elif defined TCC_TARGET_RISCV32
    "riscv32"'''),
])

patch('tcc.c', [
    ('''#elif defined TCC_TARGET_RISCV32
        "riscv32"''', '''#elif defined TCC_TARGET_GPUVM
        "gpu_vm"
#elif defined TCC_TARGET_RISCV32
        "riscv32"'''),
])

patch('tccelf.c', [
    # an absolute word is R_GVM_32 or a bundle's long immediate, R_GVM_IMM32: both a relative relocation of the library
    ('''#elif defined(TCC_TARGET_RISCV32)
        case R_RISCV_32:
#endif''', '''#elif defined(TCC_TARGET_GPUVM)
        case R_GVM_32:
        case R_GVM_IMM32:
#elif defined(TCC_TARGET_RISCV32)
        case R_RISCV_32:
#endif'''),
    ('''#elif defined TCC_TARGET_RISCV32
    ehdr.e_flags = EF_RISCV_FLOAT_ABI_SOFT;''', '''#elif defined TCC_TARGET_GPUVM
    ehdr.e_flags = 0;
#elif defined TCC_TARGET_RISCV32
    ehdr.e_flags = EF_RISCV_FLOAT_ABI_SOFT;'''),
    # code is bundles of 16 bytes
    ('''        if (s->sh_flags & SHF_EXECINSTR)
            section_add(s, 0, 4);''', '''        if (s->sh_flags & SHF_EXECINSTR)
#ifdef TCC_TARGET_GPUVM
            section_add(s, 0, 16);
#else
            section_add(s, 0, 4);
#endif'''),
])

# va_arg: gpu_vm's variadic arguments are on the stack in 4-byte slots, an aggregate over 8 bytes by a pointer
patch('include/tccdefs.h', [
    ('''#elif defined __riscv || defined TCC_TARGET_RISCV32''', '''#elif defined TCC_TARGET_GPUVM /* (an #if here is TinyCC's build's) */
    typedef char *__builtin_va_list;
    #define __builtin_va_arg(ap,type) (*(sizeof(type) > 8 ? *(type **)((ap += 4) - 4) \\
        : (type *)((ap += (sizeof(type) + 3) & -4) - ((sizeof(type) + 3) & -4))))

#elif defined __riscv || defined TCC_TARGET_RISCV32'''),
])

# a pointer's initializer: its addend in the relocation, as RELA has it (ld.lld reads no addend from the word)
patch('tccgen.c', [
    ('''	        if (vtop->r & VT_SYM)
	          greloc(sec, vtop->sym, c, R_DATA_PTR);
	        write32le(ptr, val);''', '''	        if (vtop->r & VT_SYM)
#ifdef TCC_TARGET_GPUVM
	          greloca(sec, vtop->sym, c, R_DATA_PTR, val), val = 0;
#else
	          greloc(sec, vtop->sym, c, R_DATA_PTR);
#endif
	        write32le(ptr, val);'''),
])
