"""Builds this project's Linux image from upstream rvc's (rvc/_Nix/rvc/data-net) into
build/images/linux, which the harness's linux-net entry uses when present:

  rootfs.bin  upstream's romfs with our programs added: programs/bin/glxgears in /usr/bin, and
              everything under build/images/linux/root at the same path in the image
  dts.bin     upstream's device tree with RAM ending at 0x86000000, so the kernel leaves the
              GPU device's memory alone (docs/gpu.md)
  linux_payload.bin
              only if our kernel has been built (linux/kernel/build.sh leaves it in
              build/images/linux/Image): that kernel and the device tree where the machine
              expects them. There is no firmware in it (docs/boot.md)

    python tools/make_linux_image.py
    python tools/make_linux_image.py --save-prebuilt    # then keep this build in linux/prebuilt

Our kernel and programs are taken from build/images/linux (Image, root/) when they have been
built there, else from the copies checked in under linux/prebuilt, so the image needs no
compiler.

Needs Pillow to read upstream's PNG lanes. Files are added to the romfs in place: a new entry
is appended to the image and linked to the end of its directory's list.
"""
import os, shutil, struct, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'rvc', '_Nix', 'rvc', 'data-net')
OUT = os.path.join(ROOT, 'build', 'images', 'linux')
PROGRAMS = [('glxgears', '/usr/bin')]
PREBUILT = os.path.join(ROOT, 'linux', 'prebuilt')   # checked-in copies of Image and root/


def ours(name):
    """What the build left in OUT, or failing that the checked-in copy."""
    built = os.path.join(OUT, name)
    return built if os.path.exists(built) else os.path.join(PREBUILT, name)


def read_lanes(prefix):
    """The memory image stored as four PNGs: lane k holds word k of every 16-byte texel."""
    lanes = [Image.open(os.path.join(SRC, '%s.%s.png' % (prefix, c))).convert('RGBA').tobytes() for c in 'rgba']
    out = bytearray(len(lanes[0]) * 4)
    for k in range(4):
        for b in range(4):
            out[4 * k + b::16] = lanes[k][b::4]
    return out


def be32(data, at):
    return struct.unpack_from('>I', data, at)[0]


def checksum_fix(data, start, length, field):
    """Sets the big-endian word at `field` so the words of data[start:start+length] sum to 0."""
    struct.pack_into('>I', data, field, 0)
    total = sum(struct.unpack_from('>%dI' % (length // 4), data, start)) & 0xffffffff
    struct.pack_into('>I', data, field, (-total) & 0xffffffff)


def name_end(data, at):
    """Offset just past a romfs name (NUL-terminated, padded to 16 bytes) starting at `at`."""
    end = data.index(b'\0', at)
    return at + ((end - at) // 16 + 1) * 16


def entries(data, first):
    """Yields (header offset, name, type, spec) for the list of file headers starting at `first`."""
    at = first
    while at:
        nxt = be32(data, at)
        name = bytes(data[at + 16:data.index(b'\0', at + 16)]).decode('latin1')
        yield at, name, nxt & 7, be32(data, at + 4)
        at = nxt & ~0xf


def find_dir(data, path):
    """First file header of the directory at `path`, following hard links."""
    first = name_end(data, 16)   # the root directory's entries follow the superblock
    for part in [p for p in path.split('/') if p]:
        for at, name, kind, spec in entries(data, first):
            if name == part:
                while kind == 0:   # hard link: spec points at the real header
                    at = spec
                    kind, spec = be32(data, at) & 7, be32(data, at + 4)
                if kind != 1:
                    raise SystemExit('%s: %s is not a directory' % (path, part))
                first = spec
                break
        else:
            raise SystemExit('%s: no %s in the image' % (path, part))
    return first


def add_file(data, directory, name, content):
    first = find_dir(data, directory)
    listing = list(entries(data, first))
    for old, n, kind, _ in listing:
        if n == name:
            # ours replaces upstream's: the old entry stays in the ROM under a hidden name
            if kind != 2:
                raise SystemExit('%s/%s is in the image and not a regular file' % (directory, name))
            data[old + 16] = ord('.')
            checksum_fix(data, old, name_end(data, old + 16) - old, old + 12)
    size = be32(data, 8)
    at = (size + 15) & ~15
    del data[size:]
    data.extend(b'\0' * (at - size))
    padded_name = name.encode() + b'\0' * (16 - len(name) % 16)
    header = struct.pack('>IIII', 2 | 8, 0, len(content), 0) + padded_name   # regular file, executable, last in list
    data.extend(header + content + b'\0' * (-len(content) % 16))
    checksum_fix(data, at, len(header), at + 12)
    # link it after the directory's last entry
    last = listing[-1][0]
    struct.pack_into('>I', data, last, at | (be32(data, last) & 0xf))
    checksum_fix(data, last, name_end(data, last + 16) - last, last + 12)
    data.extend(b'\0' * (-len(data) % 1024))
    struct.pack_into('>I', data, 8, at + len(header) + len(content))
    checksum_fix(data, 0, 512, 12)


def main():
    os.makedirs(OUT, exist_ok=True)
    extra = ours('root')   # a tree of more files for the image (linux/nanox/build.sh)
    rom = read_lanes('rootfs')
    if rom[:8] != b'-rom1fs-':
        raise SystemExit('upstream rootfs is not a romfs image')
    rom = bytearray(rom[:(be32(rom, 8) + 1023) & ~1023])
    before = len(rom)
    files = [(os.path.join(ROOT, 'programs', 'bin', name), directory, name) for name, directory in PROGRAMS]
    for folder, _, names in os.walk(extra):
        directory = '/' + os.path.relpath(folder, extra).replace(os.sep, '/')
        files += [(os.path.join(folder, name), directory, name) for name in sorted(names)]
    # a file in the tree replaces the one of the same name listed above
    last = {(directory, name): path for path, directory, name in files}
    files = [(path, directory, name) for (directory, name), path in last.items()]
    for path, directory, name in files:
        add_file(rom, directory, name, open(path, 'rb').read())
    print('added %d files, %d bytes' % (len(files), sum(os.path.getsize(f[0]) for f in files)))
    open(os.path.join(OUT, 'rootfs.bin'), 'wb').write(rom)
    print('rootfs.bin: %d -> %d bytes' % (before, len(rom)))

    dtb = read_lanes('dts')
    dtb = bytearray(dtb[:be32(dtb, 4)])
    # memory@80000000 { reg = <0 0x80000000 0 size> }: two cells each for address and size
    old, new = struct.pack('>IIII', 0, 0x80000000, 0, 0x07b00000), struct.pack('>IIII', 0, 0x80000000, 0, 0x06000000)
    if dtb[:4] != b'\xd0\x0d\xfe\xed' or dtb.count(old) != 1:
        raise SystemExit('device tree: expected one memory range 0x80000000 + 0x07b00000')
    dtb[dtb.index(old):dtb.index(old) + 16] = new
    # The root device is mapped a page at a time, so it is no larger than the image needs.
    # The new text has the old one's length: the tree's offsets stay as they are.
    old, new = b',256Mi console=', b',%dMi console=' % ((len(rom) >> 20) + 1)
    if dtb.count(old) != 1 or len(new) > len(old):
        raise SystemExit('device tree: expected one root device of 256Mi in the boot arguments')
    dtb[dtb.index(old):dtb.index(old) + len(old)] = new[:-9] + b' ' * (len(old) - len(new)) + new[-9:]
    # our init in place of upstream's script, when it has been built (linux/userland/build.sh)
    old, new = b' init=/rvcinit ', b' init=/emuinit '
    if os.path.exists(os.path.join(extra, 'emuinit')):
        if dtb.count(old) != 1:
            raise SystemExit('device tree: expected init=/rvcinit in the boot arguments')
        dtb[dtb.index(old):dtb.index(old) + len(old)] = new
    open(os.path.join(OUT, 'dts.bin'), 'wb').write(dtb)
    print('dts.bin: RAM now ends at 0x86000000 (%d bytes)' % len(dtb))

    # The boot image: the kernel at +4 MiB, where it is linked to run and the machine starts,
    # and the device tree at +34 MiB, where the machine tells the kernel it is.
    kernel, payload = ours('Image'), os.path.join(OUT, 'linux_payload.bin')
    if os.path.exists(kernel):
        image = open(kernel, 'rb').read()
        at, tree_at = 0x400000, 0x2200000
        if image[0x38:0x3c] != b'RSC\x05':
            raise SystemExit('expected a RISC-V kernel image in ' + kernel)
        if at + len(image) > tree_at - 0x400000:
            raise SystemExit('kernel image too large for the device tree at +34 MiB')
        open(payload, 'wb').write(bytes(at) + image + bytes(tree_at - at - len(image)) + bytes(dtb))
        print('linux_payload.bin: our kernel, device tree (%d bytes)' % (tree_at + len(dtb)))
        print('kernel from %s, programs from %s' % (os.path.relpath(kernel, ROOT), os.path.relpath(extra, ROOT)))
    elif os.path.exists(payload):
        os.remove(payload)


def save_prebuilt():
    """Replaces the checked-in kernel and programs with the ones built into OUT."""
    for name in ('Image', 'root'):
        if not os.path.exists(os.path.join(OUT, name)):
            raise SystemExit('build/images/linux/%s is missing: build it first' % name)
    os.makedirs(PREBUILT, exist_ok=True)
    shutil.copyfile(os.path.join(OUT, 'Image'), os.path.join(PREBUILT, 'Image'))
    shutil.rmtree(os.path.join(PREBUILT, 'root'), ignore_errors=True)
    shutil.copytree(os.path.join(OUT, 'root'), os.path.join(PREBUILT, 'root'))
    print('linux/prebuilt now holds this build')


if __name__ == '__main__':
    main()
    if '--save-prebuilt' in sys.argv:
        save_prebuilt()
