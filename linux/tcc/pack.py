"""Puts what the examples take of each archive first in it, and lists those pages for the
guest's compiler, mapped.c (docs/system/cc.md, "Fewer pages of the ROM"). The index keeps its order,
so the linker writes the same file.
    python3 pack.py TCC STAGE EXAMPLES OUT.h      (TCC: the compiler built for this computer)"""
import os, re, subprocess, sys, tempfile

WINDOW = 32      # pages the kernel reads of the ROM with one copy (linux/kernel/stall_hook.py)
WINDOWS = 8      # as mapped.c's table has room for
PAGE = 4096


def parts(data):
    """An archive's members in the file's order: (place, bytes with the header and the padding)."""
    assert data[:8] == b'!<arch>\n', 'not an archive'
    at, out = 8, []
    while at + 60 <= len(data):
        assert data[at + 58:at + 60] == b'`\n', 'no member at %d' % at
        size = int(data[at + 48:at + 58])
        end = min(at + 60 + size + (size & 1), len(data))
        out.append((at, data[at:end]))
        at = end
    return out


def name(member):
    return member[:16].rstrip()


def numbered(data):
    """The same archive with every member named by its number: two of an archive may have one name."""
    out, k = [data[:8]], 0
    for at, member in parts(data):
        if name(member) in (b'/', b'//'):
            out.append(member)
        else:
            out.append(b'%-16s' % (b'm%d/' % k) + member[16:])
        k += 1
    return b''.join(out)


def taken(tcc, stage, source, libs, tmp):
    """The members a program takes of each archive, in the linker's order: {archive: [number, ...]}."""
    lib = stage + '/usr/lib/'
    command = [tcc, '-vv', '-static', '-mfpu', '-nostdlib', '-nostdinc', '-I' + stage + '/usr/include',
               '-I' + lib + 'tcc/include', lib + 'crt1.o', lib + 'crti.o', source] + \
              [os.path.join(tmp, os.path.basename(l)) for l in libs] + [lib + 'crtn.o', '-o', os.path.join(tmp, 'out')]
    run = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if run.returncode:
        raise SystemExit('pack.py: %s does not link here:\n%s' % (source, run.stdout))
    got, now = {}, None
    for line in run.stdout.splitlines():
        if line.startswith('-> '):
            now = os.path.basename(line[3:].strip())
        elif now and re.match(r'   -> m\d+/', line):
            got.setdefault(now, []).append(int(line[7:].rstrip('/')))
    return got


def packed(data, tiers):
    """The archive with the members of each tier first, and the page each window ends at."""
    members = parts(data)
    lead = [k for k, (_, m) in enumerate(members) if name(m) in (b'/', b'//')]
    assert lead == list(range(len(lead))) and members and name(members[0][1]) == b'/', 'no index first'
    order, ends, at = lead[:], [], 8 + sum(len(members[k][1]) for k in lead)
    start = 0   # the page the window being filled starts at
    for tier in tiers:
        for k in tier:
            order.append(k)
            at += len(members[k][1])
        # a tier ends its window (the next tier is not always wanted), and so does a window that is full
        last = (at + PAGE - 1) // PAGE
        while start < last:
            start = min(start + WINDOW, last)
            ends.append(start)
    first = set(order)
    order += [k for k in range(len(members)) if k not in first]
    moved, out, at = {}, [data[:8]], 8
    for k in order:
        moved[members[k][0]] = at
        # (no date or owner, as `ar D` writes it: the same sources are then the same archive)
        out.append(members[k][1][:16] + b'%-12s%-6s%-6s%-8s' % (b'0', b'0', b'0', b'644') + members[k][1][48:])
        at += len(members[k][1])
    index = bytearray(out[1])
    count = int.from_bytes(index[60:64], 'big')
    for i in range(count):
        place = 64 + 4 * i
        index[place:place + 4] = moved[int.from_bytes(index[place:place + 4], 'big')].to_bytes(4, 'big')
    out[1] = bytes(index)
    return b''.join(out), ends[:WINDOWS]


def main():
    tcc, stage, examples, header = sys.argv[1:5]
    libs = [stage + '/usr/lib/' + l for l in ('libgles.a', 'libnano-X.a', 'libhlsl.a', 'libmcw.a', 'libc.a', 'tcc/libtcc1.a')]
    sources = sorted(os.path.join(examples, f) for f in os.listdir(examples) if f.endswith('.c'))
    with tempfile.TemporaryDirectory() as tmp:
        for l in libs:
            open(os.path.join(tmp, os.path.basename(l)), 'wb').write(numbered(open(l, 'rb').read()))
        took = [taken(tcc, stage, s, libs, tmp) for s in sources]
    lines = []
    for l in libs:
        base = os.path.basename(l)
        # the example that takes least of all the archives (one that only prints) stands for
        # what any program takes: that first, then what the others take besides
        lists = [t.get(base, []) for t in sorted(took, key=lambda t: sum(len(v) for v in t.values()))]
        least, more = lists[0], []
        for one in lists[1:]:
            more += [k for k in one if k not in least and k not in more]
        data, ends = packed(open(l, 'rb').read(), [least, more])
        open(l, 'wb').write(data)
        lines.append('\t{ "%s", %d, { %s } },\n' % (base, len(data), ', '.join(str(e) for e in ends)))
        print('pack.py: %-12s %3d members the smallest example takes, %3d more the others do; windows end at pages %s of %d' %
              (base, len(least), len(more), ends, (len(data) + PAGE - 1) // PAGE))
    open(header, 'w').write(''.join(lines))


main()
