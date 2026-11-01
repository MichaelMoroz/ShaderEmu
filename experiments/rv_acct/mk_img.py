#!/usr/bin/env python3
"""A private image with another kernel and the main checkout's ROM, untouched: same rootfs.bin (hard link), device tree and seed.
    python mk_img.py NAME IMAGEFILE      -> experiments/rv_acct/logs/img_NAME"""
import os, shutil, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, '..', '..'))
MAIN = subprocess.run(['git', '-C', TREE, 'worktree', 'list', '--porcelain'], capture_output=True, text=True).stdout.split('\n')[0].split(' ', 1)[1]
src = os.path.join(MAIN, 'build', 'images', 'nommu')
name, kernel = sys.argv[1], sys.argv[2]
d = os.path.join(HERE, 'logs', 'img_' + name)
os.makedirs(d, exist_ok=True)
TREE_AT = 0x3e00000
p = open(os.path.join(src, 'linux_payload.bin'), 'rb').read()
k = open(kernel, 'rb').read()
old = open(os.path.join(src, 'Image'), 'rb').read()
AT = 0x2000000
assert p[AT:AT + len(old)] == old, 'the payload does not hold its Image at 0x2000000'
assert AT + len(k) < TREE_AT
tail = p[AT + len(old):TREE_AT]
assert not any(tail), 'something lies between the kernel and the tree'
open(os.path.join(d, 'linux_payload.bin'), 'wb').write(bytes(AT) + k + bytes(TREE_AT - AT - len(k)) + p[TREE_AT:])
shutil.copy(kernel, os.path.join(d, 'Image'))
shutil.copy(os.path.join(src, 'dts.bin'), d)
r = os.path.join(d, 'rootfs.bin')
if not os.path.exists(r): os.link(os.path.join(src, 'rootfs.bin'), r)
print(d, len(k))
