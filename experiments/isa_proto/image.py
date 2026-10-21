"""An image for the machine's own instructions: a no-MMU image's device tree and ROM (or a ROM given), and the
kernel xlat_kernel.c made (kernel/build.sh) in its payload at 0x80400000.
   python experiments/isa_proto/image.py BASE_IMAGE OUT [ROM]      (then rvc_cpu --native --image OUT)"""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
base, out = sys.argv[1], sys.argv[2]
rom = sys.argv[3] if len(sys.argv) > 3 else os.path.join(base, 'rootfs.bin')
KERNEL_AT, TREE_AT = 0x400000, 0x2200000   # in the payload, which is loaded at 0x80000000
payload = open(os.path.join(base, 'linux_payload.bin'), 'rb').read()
tree = bytearray(payload[TREE_AT:])
# the root device as large as this ROM (make_linux_image.py sizes it so): the text keeps its length
at = tree.index(b'phram.phram=root,0x40000000,')
end = tree.index(b'Mi console=', at)
old = bytes(tree[at:end])
new = b'phram.phram=root,0x40000000,%d' % ((os.path.getsize(rom) >> 20) + 1)
assert len(new) <= len(old), 'the ROM size does not fit the boot arguments'
tree[at:end] = new.rjust(len(old))   # (spaces before it: a shorter size keeps the offsets)
tree = bytes(tree)
kernel = open(os.path.join(ROOT, 'build', 'isa', 'kernel.bin'), 'rb').read()
assert KERNEL_AT + len(kernel) <= TREE_AT, 'the kernel (%d bytes) reaches the device tree' % len(kernel)
os.makedirs(out, exist_ok=True)
body = payload[:KERNEL_AT] + kernel
open(os.path.join(out, 'linux_payload.bin'), 'wb').write(body + bytes(TREE_AT - len(body)) + tree)
open(os.path.join(out, 'dts.bin'), 'wb').write(tree)
target = os.path.join(out, 'rootfs.bin')
if os.path.exists(target):
    os.remove(target)
try:
    os.link(rom, target)   # (a ROM is hundreds of megabytes: the same file where it can be)
except OSError:
    shutil.copyfile(rom, target)
print(out, ': kernel %d bytes, ROM %s' % (len(kernel), rom))
