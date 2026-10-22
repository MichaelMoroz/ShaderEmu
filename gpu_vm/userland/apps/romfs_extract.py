#!/usr/bin/env python3
"""The RISC-V image's data, out of its romfs ROM (the image tools/make_linux_image.py and linux/nommu/image.py make:
upstream rvc's root with ours over it), for apps/image.sh: every file that is not a program (bFLT, ELF), under the
folders a program reads data from (/usr/share, /usr/lib, /root, /examples ...; not /etc, /bin, /sbin, /dev).
Terminal descriptions, file's magic, QuickJS's examples, the desktop's media are there and in no build of ours.
    python3 romfs_extract.py ROOTFS.BIN OUT_DIR
"""
import os, struct, sys

SKIP_TOP = {'etc', 'bin', 'sbin', 'dev', 'proc', 'sys', 'tmp', 'init', 'emuinit', 'lib', 'linuxrc', 'var', 'run', 'mnt'}


def main():
    rom, out = open(sys.argv[1], 'rb').read(), sys.argv[2]
    if rom[:8] != b'-rom1fs-':
        sys.exit('not a romfs image')
    def name_at(at):
        end = rom.index(b'\0', at)
        return rom[at:end].decode('latin-1'), (end - at + 16) & ~15
    vol, n = name_at(16)
    first = 16 + n
    files = links = 0
    pathof, pending = {}, []   # (a hard link to a program: a link to its path, once every path is known)
    def walk(at, path, seen):
        nonlocal files, links
        while at:
            nxt, spec, size = struct.unpack_from('>III', rom, at)
            kind = nxt & 7
            name, n = name_at(at + 16)
            data = at + 16 + n
            if name not in ('.', '..'):
                p = path + '/' + name
                pathof.setdefault(at, p)
                top = p.split('/')[1]
                if top not in SKIP_TOP:
                    real = at
                    if kind == 0:   # a hard link: the file it names
                        real = spec
                        rn, kind, size = None, struct.unpack_from('>I', rom, real)[0] & 7, struct.unpack_from('>I', rom, real + 8)[0]
                        data = real + 16 + name_at(real + 16)[1]
                    dst = os.path.join(out, p.lstrip('/'))
                    if kind == 1 and real not in seen:
                        os.makedirs(dst, exist_ok=True)
                        walk(spec & ~15, p, seen | {real})
                    elif kind == 2:
                        body = rom[data:data + size]
                        if body[:4] in (b'bFLT', b'\x7fELF') and real != at:
                            pending.append((dst, real))
                        elif body[:4] not in (b'bFLT', b'\x7fELF'):
                            os.makedirs(os.path.dirname(dst), exist_ok=True)
                            with open(dst, 'wb') as f:
                                f.write(body)
                            if nxt & 8:
                                os.chmod(dst, 0o755)
                            files += 1
                    elif kind == 3:
                        to = rom[data:data + size].rstrip(b'\0').decode('latin-1')
                        if not to.endswith('busybox'):   # (busybox's applets: ours)
                            os.makedirs(os.path.dirname(dst), exist_ok=True)
                            if not os.path.lexists(dst):
                                os.symlink(to, dst)
                            links += 1
            at = nxt & ~15
    walk(first, '', set())
    for dst, real in pending:
        to = pathof.get(real)
        if to and not to.endswith('busybox') and not os.path.lexists(dst):
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            os.symlink(to, dst)
            links += 1
    print('romfs_extract: %s: %d files, %d links into %s' % (vol, files, links, out))


if __name__ == '__main__':
    main()
