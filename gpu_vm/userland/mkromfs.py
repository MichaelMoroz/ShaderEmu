#!/usr/bin/env python3
"""A romfs image (the kernel's fs/romfs, as genromfs writes it) from a gen_init_cpio list (rootfs.sh's
initramfs.list: dir, file, slink, nod lines), for the root on the ROM disk gvmrom (gpu_vm/userland/README.md).
    python3 mkromfs.py LIST IMAGE
romfs keeps no owners or modes: a file with any x bit is executable (0755), others 0644, directories 0755."""
import struct, sys


def pad16(n):
    return (n + 15) & ~15


class Node:
    def __init__(self, kind, name, spec=0, data=b'', exe=False):
        self.kind, self.name, self.spec, self.data, self.exe = kind, name, spec, data, exe
        self.children = {}   # a directory's, by name, in the order made
        self.at = 0


def parse(path):
    root = Node(1, '')
    def node_at(p):
        n = root
        for part in [x for x in p.split('/') if x]:
            n = n.children[part]
        return n
    def add(p, node):
        parent, _, leaf = p.rstrip('/').rpartition('/')
        node.name = leaf
        node_at(parent).children[leaf] = node
    for line in open(path):
        w = line.split()
        if not w or w[0].startswith('#'):
            continue
        if w[0] == 'dir':
            if w[1] != '/':
                add(w[1], Node(1, ''))
        elif w[0] == 'file':
            add(w[1], Node(2, '', data=open(w[2], 'rb').read(), exe=int(w[3], 8) & 0o111 != 0))
        elif w[0] == 'slink':
            add(w[1], Node(3, '', data=w[2].encode() + b'\0'))
        elif w[0] == 'nod':
            add(w[1], Node(5 if w[5] == 'c' else 4, '', spec=(int(w[6]) << 16) | int(w[7])))
        else:
            raise SystemExit('mkromfs: what is ' + line.strip())
    return root


def header_len(name):
    return 16 + pad16(len(name.encode()) + 1)


def layout(root):
    """Offsets: after the superblock the root's list ("." first: the root's inode is the first header), each
    directory's list after its header's, depth first."""
    order = []
    def walk(d, parent):
        # the root's "." is the root itself (a directory whose list starts at itself); elsewhere "." and ".." are hard
        # links to the directory's header and its parent's
        dot = Node(1, '.') if d is parent else Node(0, '.')
        dotdot = Node(0, '..')
        dot.target, dotdot.target = d, parent
        d.list = [dot, dotdot] + list(d.children.values())
        if d is parent:
            dot.list = d.list
        for e in d.list:
            order.append(e)
            if e.kind == 1 and e is not dot:
                walk(e, d)
    walk(root, root)
    at = header_len('rom gpuvm')
    for n in order:
        n.at = at
        at += header_len(n.name) + pad16(len(n.data))
    root.at = root.list[0].at
    return order, at


def checksum_fix(buf, start, length, slot):
    s = sum(struct.unpack_from('>%dI' % (length // 4), buf, start)) & 0xffffffff
    struct.pack_into('>I', buf, slot, (-s) & 0xffffffff)


def main():
    root = parse(sys.argv[1])
    order, size = layout(root)
    img = bytearray(pad16(size) + (-pad16(size) % 1024))
    vol = 'rom gpuvm'
    img[0:8] = b'-rom1fs-'
    struct.pack_into('>II', img, 8, len(img), 0)
    img[16:16 + len(vol)] = vol.encode()
    for d in [root] + [n for n in order if n.kind == 1 and n is not root.list[0]]:
        for i, e in enumerate(d.list):
            nxt = d.list[i + 1].at if i + 1 < len(d.list) else 0
            spec = e.target.at if e.kind == 0 else e.list[0].at if e.kind == 1 else e.spec
            struct.pack_into('>IIII', img, e.at, nxt | e.kind | (8 if e.exe or e.kind == 1 else 0), spec, len(e.data), 0)
            nb = e.name.encode()
            img[e.at + 16:e.at + 16 + len(nb)] = nb
            hl = header_len(e.name)
            checksum_fix(img, e.at, hl, e.at + 12)
            img[e.at + hl:e.at + hl + len(e.data)] = e.data
    checksum_fix(img, 0, min(512, len(img)), 12)
    open(sys.argv[2], 'wb').write(img)
    print('mkromfs: %s, %d bytes, %d entries' % (sys.argv[2], len(img), len(order)))


if __name__ == '__main__':
    main()
