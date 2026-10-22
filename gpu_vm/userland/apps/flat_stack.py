#!/usr/bin/env python3
"""A program's stack as the RISC-V image gives it (linux/nommu/flat.py): 1 MB to the names of flat.py's BIG (the
games, nxweb, nxide, imdemo ...), 256 KB to every other program; never less than the program asked for when it was
linked. Run by toolchain.sh's riscv32-linux-strip on the program it installs (the name it is installed under).
    python3 flat_stack.py FLAT_PY PROGRAM
"""
import os, re, struct, sys

flat_py, prog = sys.argv[1], sys.argv[2]
src = open(flat_py).read()
m = re.search(r'^BIG = \{(.*?)\}', src, re.S | re.M)
big = set(re.findall(r"'([^']+)'", m.group(1))) if m else set()
with open(prog, 'r+b') as f:
    head = f.read(64)
    if head[:4] != b'bFLT':
        sys.exit(0)
    want = 0x100000 if os.path.basename(prog) in big else 0x40000
    have = struct.unpack_from('>I', head, 24)[0]
    if have < want:
        f.seek(24)
        f.write(struct.pack('>I', want))
