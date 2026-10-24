"""Builds this project's Linux image from upstream rvc's (rvc/_Nix/rvc/data-net) into
build/images/linux, which the harness's linux-net entry uses when present:

  rootfs.bin  upstream's romfs with our programs added: programs/bin/glxgears in /usr/bin, and
              everything under build/images/linux/root at the same path in the image
  dts.bin     upstream's device tree with RAM ending at 0x86000000, so the kernel leaves the
              GPU device's memory alone (docs/machine/gpu.md)
  linux_payload.bin
              only if our kernel has been built (linux/kernel/build.sh leaves it in
              build/images/linux/Image): that kernel and the device tree where the machine
              expects them. There is no firmware in it (docs/machine/boot.md)

    python tools/make_linux_image.py
    python tools/make_linux_image.py --save-prebuilt    # then keep this build in linux/prebuilt

Our kernel and programs are taken from build/images/linux (Image, root/) when they have been
built there, else from the copies checked in under linux/prebuilt, so the image needs no
compiler.

Needs Pillow to read upstream's PNG lanes. Files are added to the romfs in place: a new entry
is appended to the image and linked to the end of its directory's list. A folder the ROM
lacks is made the same way, and NAME.romlink (a line with a path in the image) becomes NAME,
a hard link to that file.
"""
import os, shutil, struct, sys, time
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'rvc', '_Nix', 'rvc', 'data-net')
OUT = os.path.join(ROOT, 'build', 'images', 'linux')
PROGRAMS = [('glxgears', '/usr/bin'), ('mctest', '/usr/bin')]   # mctest: programs/mc (docs/machine/multicore.md)
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


def append(data, kind, spec, name, content=b''):
    """A new file header at the image's end, last in its list; returns its offset."""
    size = be32(data, 8)
    at = (size + 15) & ~15
    del data[size:]
    data.extend(b'\0' * (at - size))
    padded_name = name.encode() + b'\0' * (16 - len(name) % 16)
    header = struct.pack('>IIII', kind, spec, len(content), 0) + padded_name
    data.extend(header + content + b'\0' * (-len(content) % 16))
    checksum_fix(data, at, len(header), at + 12)
    data.extend(b'\0' * (-len(data) % 1024))
    struct.pack_into('>I', data, 8, at + len(header) + len(content))
    checksum_fix(data, 0, 512, 12)
    return at


def link_after(data, last, at):
    """Makes the header at `at` the one after `last` in a directory's list."""
    struct.pack_into('>I', data, last, at | (be32(data, last) & 0xf))
    checksum_fix(data, last, name_end(data, last + 16) - last, last + 12)


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
    link_after(data, listing[-1][0], append(data, 2 | 8, 0, name, content))   # regular file, executable


def has_dir(data, path):
    try:
        find_dir(data, path)
        return True
    except SystemExit:
        return False


def add_dir(data, parent, name):
    """A new directory in `parent`: its header, then "." and ".." as hard links, as genromfs writes them."""
    first = find_dir(data, parent)
    listing = list(entries(data, first))
    # the parent's own header: its "." is that header (the root) or a hard link to it
    above = listing[0][0] if listing[0][2] == 1 else listing[0][3]
    at = append(data, 1 | 8, 0, name)
    dot = append(data, 0, at, '.')
    struct.pack_into('>I', data, at + 4, dot)   # the directory's list starts at "."
    checksum_fix(data, at, name_end(data, at + 16) - at, at + 12)
    link_after(data, dot, append(data, 0, above, '..'))
    link_after(data, listing[-1][0], at)


def add_link(data, directory, name, target):
    """A second name for the file at `target` (a hard link: no bytes of its own)."""
    folder, _, leaf = target.rpartition('/')
    for at, n, kind, spec in entries(data, find_dir(data, folder or '/')):
        if n == leaf:
            listing = list(entries(data, find_dir(data, directory)))
            for old, other, other_kind, _ in listing:   # a file of upstream's of that name is hidden, as add_file does
                if other == name and other_kind != 1:
                    data[old + 16] = ord('.')
                    checksum_fix(data, old, name_end(data, old + 16) - old, old + 12)
            link_after(data, listing[-1][0], append(data, 0, spec if kind == 0 else at, name))
            return
    raise SystemExit('%s: nothing to link %s/%s to' % (target, directory, name))


ICON_MARK = b'NXICON1\0'


def with_icon(share, name, content):
    """A program with its icon at its end: 32 x 32 then 16 x 16 pixels of three bytes (magenta
    where it is clear), then the mark (linux/nanox/scr_shaderemu.c draws it from there). The
    pictures are the sheets' cells for the program's name (tools/make_icons.py); a program
    that has an icon already, or no cell, is left as it is."""
    index = os.path.join(share, 'icons.idx')
    if not content.startswith(b'\x7fELF') or content.endswith(ICON_MARK) or not os.path.exists(index):
        return content
    cells = dict(line.split() for line in open(index))
    if name not in cells:
        return content
    cell, out = int(cells[name]), b''
    for size in (32, 16):
        sheet = Image.open(os.path.join(share, 'icons%d.ppm' % size)).convert('RGB')
        out += sheet.crop((cell % 16 * size, cell // 16 * size, cell % 16 * size + size, cell // 16 * size + size)).tobytes()
    return content + out + ICON_MARK


DESKTOP = '/root/Desktop'
WITH_PLACES = ('nxnet', 'nxsettings')
PLACES = [('Home', 'home', 'nxfiles /root'), ('Pictures', 'pictures', 'nxfiles /root/Pictures'),
          ('Music', 'music', 'nxfiles /root/Music'), ('Videos', 'videos', 'nxfiles /root/Videos'),
          ('Persistence', 'persistence', 'nxfiles /persistence')]   # (the nx script makes the folder: docs/system/persist.md)


DOCS = '/usr/share/doc/shaderemu'   # the documentation, which the desktop's Docs folder opens
GPU_VM_DOCS = (('GPU_VM.md', "gpu_vm, the machine's own instruction set"), ('platform.md', 'gpu_vm as a whole machine: devices, workers, drivers'))


def documentation():
    """The repository's documentation for the machine's own reader (the editor reads Markdown: docs/system/nanox.md,
    "The editor's reader"), at DOCS: docs/ as it is but for its plans, and gpu_vm's two docs for users in gpu_vm/. The index
    (README.md) leaves the plans out and names gpu_vm's. A list of (bytes, folder, name), read anew at every build."""
    top, files = os.path.join(ROOT, 'docs'), []
    for folder, dirs, names in os.walk(top):
        dirs[:] = sorted(d for d in dirs if d != 'plans')
        rel = os.path.relpath(folder, top).replace(os.sep, '/')
        for name in sorted(n for n in names if n.endswith('.md')):
            text = open(os.path.join(folder, name), 'rb').read()
            if rel == '.' and name == 'README.md':
                lines, out, skip = text.decode('utf-8').split('\n'), [], False
                for line in lines:
                    if line.startswith('## '):
                        skip = line.strip() == '## plans'
                    if not skip:
                        out.append(line)
                while out and not out[-1].strip():
                    out.pop()
                out += ['', '## gpu_vm', ''] + ['- [%s](gpu_vm/%s) – %s' % (n, n, title) for n, title in GPU_VM_DOCS] + ['']
                text = '\n'.join(out).encode('utf-8')
            files.append((text, DOCS + ('' if rel == '.' else '/' + rel), name))
    for name, _ in GPU_VM_DOCS:
        files.append((open(os.path.join(ROOT, 'gpu_vm', name), 'rb').read(), DOCS + '/gpu_vm', name))
    # (and test_all.py nxedit-read's page of every kind of Markdown)
    files.append((open(os.path.join(ROOT, 'linux', 'apps', 'md-test.md'), 'rb').read(), '/usr/share', 'md-test.md'))
    return files


def desktop_shortcuts(extra):
    """The desktop as the image starts with it (linux/apps/nxfiles.c): a shortcut for every
    program of the Start menu (the nxapps.* files) but the System folder's, and for the places
    of PLACES whose folder the image has; and where each stands, a column a folder of the menu.
    The machine's own programs (WITH_PLACES) stand with the places, in the first column."""
    share = os.path.join(extra, 'usr', 'share')
    if not os.path.isdir(share):
        return []
    columns = {}   # a column of icons: the places, then each folder of the menu
    for label, icon, run in PLACES:
        folder = run.split(' ', 1)[1]
        if folder in ('/root', '/persistence') or os.path.isdir(os.path.join(extra, *folder.split('/'))):
            columns.setdefault('', []).append((label, 'name=%s\nicon=%s\nrun=%s\n' % (label, icon, run)))
    for registry in sorted(n for n in os.listdir(share) if n.startswith('nxapps.')):
        for line in open(os.path.join(share, registry)).read().splitlines():
            path, _, run = line.partition('=')
            folder, _, label = path.rpartition('/')
            if run and label and folder != 'System':
                folder = '' if run.split()[0] in WITH_PLACES else folder or 'Other'
                columns.setdefault(folder, []).append((label, 'name=%s\nrun=%s\n' % (label, run)))
    # the documentation (documentation()) in a column of its own, the last: no other icon moves for it
    columns.setdefault(' docs', []).append(('Docs', 'name=Docs\nicon=documents\nrun=nxfiles %s\n' % DOCS))
    files, places, column = [], [], 0
    for folder in columns:
        for row, (label, text) in enumerate(columns[folder]):
            # 7 icons down a column: what an 800 x 600 screen has room for above its bar
            name = label.replace('/', '-') + '.app'
            if any(name == other for _, _, other in files):
                raise SystemExit('two shortcuts on the desktop would be called %s' % name)
            places.append('%d %d %s\n' % ((column + row // 7) * 76, 4 + row % 7 * 70, name))
            files.append((text.encode(), DESKTOP, name))
        column += (len(columns[folder]) + 6) // 7
    files.append((''.join(places).encode(), DESKTOP, '.places'))
    return files


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
    files += documentation()
    files += desktop_shortcuts(extra)
    # a file in the tree replaces the one of the same name listed above
    last = {(directory, name): path for path, directory, name in files}
    files = [(path, directory, name) for (directory, name), path in last.items()]
    files.sort(key=lambda f: f[2].endswith('.romlink'))   # links last: what they name is in by then
    # romfs keeps no dates: each file's goes into a list of its own, which Properties reads
    now, dates = int(time.time()), []
    for path, directory, name in files:
        shown = name[:-8] if name.endswith('.romlink') else name
        dates.append('%d %s/%s\n' % (now if isinstance(path, bytes) else int(os.path.getmtime(path)), directory.rstrip('/'), shown))
    files.append((''.join(sorted(dates, key=lambda line: line.split(' ', 1)[1])).encode(), '/usr/share', 'rom-dates'))
    for path, directory, name in files:
        # a folder the ROM lacks is made, each part in turn
        parts = [p for p in directory.split('/') if p]
        for depth in range(len(parts)):
            above = '/' + '/'.join(parts[:depth])
            if not has_dir(rom, above + '/' + parts[depth]):
                add_dir(rom, above, parts[depth])
        if isinstance(path, bytes):     # made here, not a file of the tree
            add_file(rom, directory, name, path)
        elif name.endswith('.romlink'):   # a file that only names another: a hard link to it
            add_link(rom, directory, name[:-8], open(path).read().strip())
        else:
            content = open(path, 'rb').read()
            if directory in ('/usr/bin', '/bin'):
                content = with_icon(os.path.join(extra, 'usr', 'share'), name, content)
            add_file(rom, directory, name, content)
    print('added %d files, %d bytes' % (len(files), sum(len(f[0]) if isinstance(f[0], bytes) else os.path.getsize(f[0]) for f in files)))
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
