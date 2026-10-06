"""Builds this project's Linux image from upstream rvc's (rvc/_Nix/rvc/data-net) into
build/images/linux, which the harness's linux-net entry uses when present:

  rootfs.bin  upstream's romfs with our programs added to /usr/bin (programs/bin/glxgears)
  dts.bin     upstream's device tree with RAM ending at 0x87000000, so the kernel leaves the
              GPU device's memory alone (docs/gpu.md)

    python tools/make_linux_image.py

Needs Pillow to read upstream's PNG lanes. Files are added to the romfs in place: a new entry
is appended to the image and linked to the end of its directory's list.
"""
import os, struct, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'rvc', '_Nix', 'rvc', 'data-net')
OUT = os.path.join(ROOT, 'build', 'images', 'linux')
PROGRAMS = [('glxgears', '/usr/bin')]


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
    if any(n == name for _, n, _, _ in listing):
        raise SystemExit('%s/%s is already in the image' % (directory, name))
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
    rom = read_lanes('rootfs')
    if rom[:8] != b'-rom1fs-':
        raise SystemExit('upstream rootfs is not a romfs image')
    rom = bytearray(rom[:(be32(rom, 8) + 1023) & ~1023])
    before = len(rom)
    for name, directory in PROGRAMS:
        path = os.path.join(ROOT, 'programs', 'bin', name)
        add_file(rom, directory, name, open(path, 'rb').read())
        print('added %s/%s (%d bytes)' % (directory, name, os.path.getsize(path)))
    open(os.path.join(OUT, 'rootfs.bin'), 'wb').write(rom)
    print('rootfs.bin: %d -> %d bytes' % (before, len(rom)))

    dtb = read_lanes('dts')
    dtb = bytearray(dtb[:be32(dtb, 4)])
    # memory@80000000 { reg = <0 0x80000000 0 size> }: two cells each for address and size
    old, new = struct.pack('>IIII', 0, 0x80000000, 0, 0x07b00000), struct.pack('>IIII', 0, 0x80000000, 0, 0x07000000)
    if dtb[:4] != b'\xd0\x0d\xfe\xed' or dtb.count(old) != 1:
        raise SystemExit('device tree: expected one memory range 0x80000000 + 0x07b00000')
    dtb[dtb.index(old):dtb.index(old) + 16] = new
    open(os.path.join(OUT, 'dts.bin'), 'wb').write(dtb)
    print('dts.bin: RAM now ends at 0x87000000 (%d bytes)' % len(dtb))


if __name__ == '__main__':
    main()
