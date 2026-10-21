"""The ROM for the machine's own instructions: a no-MMU image's romfs written anew, every RISC-V flat program in
it replaced by the machine's from a root flat_native.py made (same path), the files it hid left out.
   python3 rom.py IN_ROOTFS NATIVE_ROOT OUT_ROOTFS"""
import os, struct, sys

data = open(sys.argv[1], 'rb').read()
native_root, out_path = sys.argv[2], sys.argv[3]
be32 = lambda d, at: struct.unpack_from('>I', d, at)[0]
assert data[:8] == b'-rom1fs-', 'not a romfs image'


def name_at(at):
    end = data.index(b'\0', at + 16)
    return data[at + 16:end].decode('latin-1'), (end + 16) & ~15


def listing(first):
    """A directory's entries from its first header: (offset, name, kind, exec, spec, size, content offset)."""
    at = first
    while at:
        nxt, spec, size = be32(data, at), be32(data, at + 4), be32(data, at + 8)
        name, body = name_at(at)
        yield at, name, nxt & 7, (nxt >> 3) & 1, spec, size, body
        at = nxt & ~15


root_first = name_at(0)[1]   # (the volume name's end: the root's first header)
paths = {}   # an original header's offset: its path, for hard links


def tree(first, path):
    """A directory as ((name, kind, exec, spec, content or children), ...), hidden replaced entries left out."""
    items = list(listing(first))
    names = {n for _, n, _, _, _, _, _ in items}
    out = []
    for at, name, kind, ex, spec, size, body in items:
        if name in ('.', '..'):
            continue
        if name.startswith('.') and any(o != name and o[1:] == name[1:] for o in names):
            continue   # (make_linux_image's add_file hid it under a dot: a newer file has its name)
        full = path.rstrip('/') + '/' + name
        paths[at] = full
        if kind == 1:
            out.append((name, 1, ex, 0, tree(spec, full)))
        elif kind in (2, 3):
            out.append((name, kind, ex, spec, data[body:body + size]))
        else:
            out.append((name, kind, ex, spec, None))
    return out


top = tree(root_first, '/')
for at, name, kind, ex, spec, size, body in listing(root_first):
    if name == '.':
        paths[at] = '/'

# the machine's programs in place of the RISC-V ones
swapped, kept = 0, []


def swap(items, path):
    global swapped
    for i, (name, kind, ex, spec, content) in enumerate(items):
        full = path.rstrip('/') + '/' + name
        if kind == 1:
            swap(content, full)
        elif kind == 2 and content[:4] == b'bFLT' and not (be32(content, 36) & 0x100):
            mine = os.path.join(native_root, full.lstrip('/'))
            if os.path.isfile(mine) and open(mine, 'rb').read(4) == b'bFLT':
                items[i] = (name, kind, ex, spec, open(mine, 'rb').read())
                swapped += 1
            else:
                kept.append(full)


swap(top, '/')

# written anew, as genromfs lays it out: a directory's headers in a chain, "." and ".." first
out = bytearray(data[:root_first])
links = []   # (header, the original target's offset)
headers = {}  # path: the new header
made = []     # every header and its length with the name


def put(kind, ex, spec, name, content=b''):
    at = len(out)
    nm = name.encode('latin-1')
    out.extend(struct.pack('>IIII', 0, spec, len(content), 0) + nm + b'\0' * (16 - len(nm) % 16))
    made.append((at, 16 + len(nm) + (16 - len(nm) % 16)))
    out.extend(content + b'\0' * (-len(content) % 16))
    struct.pack_into('>I', out, at, (ex << 3) | kind)
    return at


def chain(hs):
    for a, b in zip(hs, hs[1:]):
        struct.pack_into('>I', out, a, be32(out, a) | b)


def write_dir(items, self_header, parent_header, path):
    """The directory's list; its first header's offset."""
    dot = put(1 if self_header is None else 0, 1, 0 if self_header is None else self_header, '.')
    if self_header is None:
        struct.pack_into('>I', out, dot + 4, dot)
        self_header = dot
        headers['/'] = dot
    dotdot = put(0, 1, parent_header if parent_header is not None else dot, '..')
    hs, subs = [dot, dotdot], []
    for name, kind, ex, spec, content in items:
        full = path.rstrip('/') + '/' + name
        if kind == 1:
            h = put(1, ex, 0, name)
            subs.append((h, content, full))
        elif kind in (2, 3):
            h = put(kind, ex, 0, name, content)
        elif kind == 0:
            h = put(0, ex, 0, name)
            links.append((h, spec))
        else:
            h = put(kind, ex, spec, name)
        headers[full] = h
        hs.append(h)
    chain(hs)
    for h, content, full in subs:
        first = write_dir(content, h, self_header, full)
        struct.pack_into('>I', out, h + 4, first)
    return dot


write_dir(top, None, None, '/')
for h, target in links:
    struct.pack_into('>I', out, h + 4, headers[paths[target]])


def checksum(at, length, field):
    struct.pack_into('>I', out, field, 0)
    s = sum(struct.unpack_from('>%dI' % (length // 4), out, at)) & 0xffffffff
    struct.pack_into('>I', out, field, (-s) & 0xffffffff)


for h, length in made:   # each header's checksum, its name included, as genromfs writes it
    checksum(h, length, h + 12)
size = len(out)
out.extend(b'\0' * (-len(out) % 1024))
struct.pack_into('>I', out, 8, size)
checksum(0, min(512, size) & ~3, 12)
open(out_path, 'wb').write(out)
print('%s: %d bytes (was %d), %d programs the machine\'s, %d RISC-V flat programs left' % (out_path, len(out), len(data), swapped, len(kept)))
for k in kept[:20]:
    print('  left:', k)
