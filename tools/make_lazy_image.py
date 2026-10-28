"""An image folder that runs programs\\bin\\lazy.bin in place of the kernel (programs\\lazy\\lazy.c).

    python tools\\make_lazy_image.py OUT_ROOT [--program NAME] [--rom-kb N]

Writes OUT_ROOT\\build\\images\\linux\\{linux_payload.bin, rootfs.bin, dts.bin}: start rvc_cpu
with --image OUT_ROOT\\build\\images\\linux, or rvc_harness with OUT_ROOT as the working
directory and --image linux-net. The ROM is sixteen pages of a pattern, which the program copies from.
--program runs programs\\bin\\NAME.bin instead (programs\\copytest: with --rom-kb 1024, a longer ROM).
"""
import os, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(sys.argv[1], 'build', 'images', 'linux')
os.makedirs(out, exist_ok=True)
args = sys.argv[2:]
name = args[args.index('--program') + 1] if '--program' in args else 'lazy'
rom = int(args[args.index('--rom-kb') + 1]) * 1024 if '--rom-kb' in args else 65536
program = open(os.path.join(root, 'programs', 'bin', name + '.bin'), 'rb').read()
tree = open(os.path.join(root, 'build', 'images', 'linux', 'dts.bin'), 'rb').read()
at, tree_at = 0x2000000, 0x3e00000   # the kernel's place and the tree's (tools\make_linux_image.py)
open(os.path.join(out, 'linux_payload.bin'), 'wb').write(bytes(at) + program + bytes(tree_at - at - len(program)) + tree)
# (a pattern after the name: the program copies pages of it)
open(os.path.join(out, 'rootfs.bin'), 'wb').write(b'-rom1fs-' + bytes((i * 197 + (i >> 8) * 31 + 7) & 255 for i in range(8, rom)))
open(os.path.join(out, 'dts.bin'), 'wb').write(tree)
print(name, 'image in', out, '(%d bytes of program)' % len(program))
