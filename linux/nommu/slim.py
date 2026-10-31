"""The no-MMU image's romfs written anew without what that machine can neither run nor read
(docs/system/nommu.md, "The ROM"): upstream's dynamic ELF programs and libraries (the kernel loads
only flat programs), the copies of upstream's files that ours hid, /usr/local's i386 C compiler,
and data only those programs read. A link to something dropped goes with it. A file of SQUEEZE
is stored as deflate in segments (docs/system/media.md) when the program that reads it inflates it.
   python linux/nommu/slim.py IMAGE_DIR"""
import os, re, struct, sys, zlib

DEAD = ('/usr/local',               # bcc and beluga, dynamic ELF writing i386 assembly (docs/system/ports.md)
        '/usr/share/screen',        # screen's encodings: screen is not built (docs/system/terminal.md)
        '/usr/share/glib-2.0',      # GLib's valgrind list
        '/etc/ssh/moduli',          # upstream sshd's: OpenSSH's sftp and Dropbear never read it
        '/lib/libgcc_s.so')         # a linker script naming the dropped libgcc_s.so.1
# a folder that only its program reads, dropped when the program is not in the image
ORPHANS = {'/usr/share/zsh': '/bin/zsh'}   # upstream zsh's functions; zsh is built only when asked
# a file: (its reader, a text in the reader that says it inflates it)
SQUEEZE = {'/usr/share/misc/magic.mgc': ('/usr/bin/file', b"cannot inflate `%s'")}   # linux/terminal/file-mdz.patch
SEGMENT = 8192


def be32(data, at):
    return struct.unpack_from('>I', data, at)[0]


def name_end(data, at):
    end = data.index(b'\0', at)
    return at + ((end - at) // 16 + 1) * 16


def checksum_fix(data, start, length, field):
    struct.pack_into('>I', data, field, 0)
    total = sum(struct.unpack_from('>%dI' % (length // 4), data, start)) & 0xffffffff
    struct.pack_into('>I', data, field, (-total) & 0xffffffff)


def segments(data):
    """MDZ1: the inflated size, the piece's, the count, count + 1 offsets, then raw deflate streams."""
    parts = []
    for i in range(0, len(data), SEGMENT):
        z = zlib.compressobj(9, zlib.DEFLATED, -15, 9)
        parts.append(z.compress(data[i:i + SEGMENT]) + z.flush())
    at, offsets = 16 + 4 * (len(parts) + 1), []
    for part in parts:
        offsets.append(at)
        at += len(part)
    offsets.append(at)
    return b'MDZ1' + struct.pack('<III', len(data), SEGMENT, len(parts)) + struct.pack('<%dI' % len(offsets), *offsets) + b''.join(parts)


def headers(data):
    """Every header reachable from the root, hidden ones too: {offset: (path, kind, spec, size, data offset)}."""
    out, todo = {}, [(name_end(data, 16), '')]
    while todo:
        at, folder = todo.pop()
        while at and at not in out:
            nxt, spec, size = be32(data, at), be32(data, at + 4), be32(data, at + 8)
            name = bytes(data[at + 16:data.index(b'\0', at + 16)]).decode('latin1')
            path = folder + '/' + name if name not in ('.', '..') else folder
            out[at] = (path, nxt & 7, spec, size, name_end(data, at + 16))
            if nxt & 7 == 1:
                todo.append((spec, path))
            at = nxt & ~0xf
    return out


def dead(data, path, kind, size, body, gone):
    if any(path == d or path.startswith(d + '/') for d in gone):
        return True
    if kind == 2 and size >= 18 and data[body:body + 4] == b'\x7fELF':
        return struct.unpack_from('<H', data, body + 16)[0] in (2, 3)   # programs and shared objects; .o files stay
    return False


def slim(rom):
    hs = headers(rom)
    by_path = {h[0]: at for at, h in hs.items() if h[0] and h[1] != 0}
    drop = {at for at, (path, kind, spec, size, body) in hs.items() if dead(rom, path, kind, size, body, DEAD)}
    gone = tuple(d for d, prog in ORPHANS.items() if prog not in by_path or by_path[prog] in drop)
    if gone:
        drop |= {at for at, (path, kind, spec, size, body) in hs.items() if dead(rom, path, kind, size, body, gone)}
    while True:   # links to what is dropped, until none is left
        more = set()
        for at, (path, kind, spec, size, body) in hs.items():
            if at in drop:
                continue
            if kind == 0 and spec in drop:
                more.add(at)
            elif kind == 3:
                target = bytes(rom[body:body + size]).decode('latin1')
                full = os.path.normpath(target if target.startswith('/') else os.path.dirname(path) + '/' + target).replace('\\', '/')
                if by_path.get(full) in drop:
                    more.add(at)
        if not more:
            break
        drop |= more
    keep = sorted(at for at in hs if at not in drop)
    squeezed = {}
    for path, (reader, mark) in SQUEEZE.items():
        at, by = by_path.get(path), by_path.get(reader)
        if at in hs and by in hs and mark in rom[hs[by][4]:hs[by][4] + hs[by][3]]:
            body, size = hs[at][4], hs[at][3]
            if not rom[body:body + 4] == b'MDZ1':
                squeezed[at] = segments(rom[body:body + size])
    first = keep[0]
    out = bytearray(rom[:first])
    where = {}
    for at in keep:
        where[at] = len(out)
        body = squeezed.get(at, rom[hs[at][4]:hs[at][4] + hs[at][3]])
        out += rom[at:hs[at][4]] + body + bytes(-len(body) % 16)
        struct.pack_into('>I', out, where[at] + 8, len(body))

    def alive(at):   # the next header in a list that is kept
        while at and at in drop:
            at = be32(rom, at) & ~0xf
        return at

    for at in keep:
        new, nxt = where[at], be32(rom, at)
        follow = alive(nxt & ~0xf)
        struct.pack_into('>I', out, new, (where[follow] if follow else 0) | (nxt & 0xf))
        if nxt & 7 in (0, 1):
            struct.pack_into('>I', out, new + 4, where[be32(rom, at + 4)])
        checksum_fix(out, new, name_end(out, new + 16) - new, new + 12)
    size = len(out)
    out += bytes(-len(out) % 1024)
    struct.pack_into('>I', out, 8, size)
    checksum_fix(out, 0, 512, 12)
    for at, body in squeezed.items():
        print('%s: %d bytes as %d, deflate in segments' % (hs[at][0], hs[at][3], len(body)))
    return out, [hs[at][0] for at in sorted(drop)], sum(hs[at][3] for at in drop)


def name_tables(out):
    """A table of each directory of 32 entries or more, at the ROM's end, its place in the directory's header's size
    (linux/nommu/patch_romfs.py): the mask, then that many + 1 words, each an entry's offset or 0, at the name's hash."""
    out = bytearray(out)
    tables = bytearray()
    start = (len(out) + 15) & ~15
    root = name_end(out, 16)
    todo, seen = [root], set()
    while todo:
        d = todo.pop()
        if d in seen:
            continue
        seen.add(d)
        entries, at = [], be32(out, d + 4) & ~15
        while at:
            nxt = be32(out, at)
            end = out.index(0, at + 16)
            name = bytes(out[at + 16:end])
            entries.append((at, name))
            if nxt & 7 == 1 and name not in (b'.', b'..'):
                todo.append(at)
            at = nxt & ~15
        if len(entries) < 32:
            continue
        mask = 63
        while mask < len(entries) * 2:
            mask = mask * 2 + 1
        tab = [0] * (mask + 1)
        for at, name in entries:
            h = 0
            for c in name:
                h = (h * 31 + c) & 0xffffffff
            while tab[h & mask]:
                h += 1
            tab[h & mask] = at
        struct.pack_into('>I', out, d + 8, start + len(tables))
        checksum_fix(out, d, name_end(out, d + 16) - d, d + 12)
        tables += struct.pack('<%dI' % (mask + 2), mask, *tab)
        tables += bytes(-len(tables) & 15)
    out += bytes(start - len(out)) + tables
    size = len(out)
    struct.pack_into('>I', out, 8, size)
    checksum_fix(out, 0, 512, 12)
    return bytes(out)


def main(image):
    path = os.path.join(image, 'rootfs.bin')
    rom = open(path, 'rb').read()
    out, dropped, total = slim(rom)
    out = name_tables(out)
    open(path, 'wb').write(out)
    # the root device's size in the boot arguments follows the ROM (tools/make_linux_image.py)
    for name in ('dts.bin', 'linux_payload.bin'):
        p = os.path.join(image, name)
        blob = open(p, 'rb').read()
        m = re.search(rb',(\d+)Mi( *) console=', blob)
        new = b',%dMi' % ((len(out) >> 20) + 1)
        text = new + b' ' * (len(m.group(0)) - len(new) - 9) + b' console='
        assert len(text) == len(m.group(0))
        open(p, 'wb').write(blob[:m.start()] + text + blob[m.end():])
    print('rootfs.bin: %d -> %d bytes, %d entries dropped (%d bytes of files)' % (len(rom), len(out), len(dropped), total))
    return dropped


if __name__ == '__main__':
    for p in main(sys.argv[1]):
        print(p)
