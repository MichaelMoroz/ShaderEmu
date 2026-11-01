#!/usr/bin/env python3
"""programs/classes in the kernel's place, for the lean tick, which has no bare machine: logs/classes_img.   python classes_img.py [C_FILE]"""
import os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SNAP = os.environ.get('AGENTS_ISA', r'C:\Users\micha\agents_isa')
L = os.environ.get('LLVM_BIN', r'C:\Program Files\LLVM\bin')
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'programs', 'classes', 'classes.c')
d = os.path.join(ROOT, 'logs', 'classes_img'); os.makedirs(d, exist_ok=True)
elf, bin_ = os.path.join(ROOT, 'logs', 'classes.elf'), os.path.join(ROOT, 'logs', 'classes.bin')
p = os.path.join(ROOT, 'programs')
subprocess.check_call([os.path.join(L, 'clang.exe'), '--target=riscv32-unknown-elf', '-march=rv32ima', '-mabi=ilp32', '-O2', '-ffreestanding', '-fno-builtin', '-nostdlib', '-static', '-fuse-ld=lld',
                       '-I', os.path.join(p, 'classes'), '-Wl,-T,' + os.path.join(p, 'lazy', 'link.ld'), os.path.join(p, 'common', 'start.S'), src, '-o', elf])
subprocess.check_call([os.path.join(L, 'llvm-objcopy.exe'), '-O', 'binary', elf, bin_])
tree = open(os.path.join(SNAP, 'img', 'dts.bin'), 'rb').read(); prog = open(bin_, 'rb').read()
at, ta = 0x2000000, 0x3e00000
open(os.path.join(d, 'linux_payload.bin'), 'wb').write(bytes(at) + prog + bytes(ta - at - len(prog)) + tree)
open(os.path.join(d, 'rootfs.bin'), 'wb').write(b'-rom1fs-' + bytes(65536 - 8))
open(os.path.join(d, 'dts.bin'), 'wb').write(tree)
print(len(prog), 'bytes of program in', d)
