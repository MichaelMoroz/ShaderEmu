"""Functions of a game's flat program at run time, for gpu_vm/PROFILE_Q3.md (both machines).

    python gpu_vm/bench_games/profile_syms.py rv BFLT ELF PCS.TXT OUT.tsv     (RISC-V: rvc_cpu RVC_PROF's pcs.txt)
    python gpu_vm/bench_games/profile_syms.py gvm ELF PROF OUT.tsv            (gpu_vm: a gvm_cpu --profile)

RISC-V: the image's bFLT has no symbols; an ELF of the same sources built apart has them. Each ELF function is found in
the bFLT by its words' 6-grams (immediates masked: the two links lay things out a little differently), as the earlier
scratch q3sym.py did; then the program's load base is the offset that puts the profile's executed words on the bFLT's.
gpu_vm: the ELF is the program's own (`.gdb`), its base from the profile's hottest bundles (hotcode_lib.find_base).
OUT.tsv: start, end (run-time addresses, hex) and name, by address.
"""
import bisect, collections, os, struct, sys
import numpy as np


def elf_funcs(fn):
    e = open(fn, 'rb').read()
    shoff, = struct.unpack_from('<I', e, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', e, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', e, shoff + i * shentsize) for i in range(shnum)]
    text = [s for s in secs if s[1] == 1 and s[2] & 4 and s[5] > 100][0]
    funcs = []
    for s in secs:
        if s[1] == 2:
            st = secs[s[6]]
            for at in range(s[4], s[4] + s[5], 16):
                nm, val, sz, info = struct.unpack_from('<IIIB', e, at)
                if info & 15 != 2 or not val or not sz:
                    continue
                end = e.index(b'\0', st[4] + nm)
                funcs.append((val, sz, e[st[4] + nm:end].decode()))
    return e, text, sorted(set(funcs))


def mask(w):
    op = w & 0x7f
    m = np.where(np.isin(op, [0x17, 0x37, 0x6f]), w & 0xfff, w)
    m = np.where(np.isin(op, [0x03, 0x13, 0x07, 0x67]), m & 0xfffff, m)
    return np.where(np.isin(op, [0x23, 0x27]), m & 0x1fff07f, m)


def grams(m, n=6):
    h = np.zeros(len(m) - n + 1, dtype=np.int64)
    for i in range(n):
        h = h * 1000003 + m[i:len(m) - n + 1 + i]
    return h


def rv_funcs(bflt, elf):
    """[(file offset, size, name)] of the ELF's functions found in the bFLT"""
    fl = open(bflt, 'rb').read()
    data_start = struct.unpack('>I', fl[12:16])[0]
    bw = np.frombuffer(fl[:data_start // 4 * 4], dtype='<u4').astype(np.int64)
    e, text, funcs = elf_funcs(elf)
    ta, to, ts = text[3], text[4], text[5]
    ew = np.frombuffer(e[to:to + ts // 4 * 4], dtype='<u4').astype(np.int64)
    bm, em = mask(bw), mask(ew)
    bg, eg = grams(bm), grams(em)
    cnt = collections.Counter(bg.tolist())
    uniq = {g: i for i, g in enumerate(bg.tolist()) if cnt[g] == 1}
    mapped = {}
    for val, sz, name in funcs:
        i0, n = (val - ta) // 4, sz // 4
        deltas = collections.Counter()
        for j in range(i0, min(i0 + n - 5, len(eg))):
            b = uniq.get(int(eg[j]))
            if b is not None:
                deltas[b - j] += 1
        if deltas:
            d = deltas.most_common(1)[0][0]
            b0 = i0 + d
            if 0 <= b0 and b0 + n <= len(bm) and n and (bm[b0:b0 + n] == em[i0:i0 + n]).mean() > 0.6:
                mapped[name] = (b0 * 4, sz)
    # the rest take the shift of their mapped neighbours in the ELF
    shifts = sorted((v, mapped[nm][0] - (v - ta)) for v, sz, nm in funcs if nm in mapped)
    ks = [x[0] for x in shifts]
    for val, sz, name in funcs:
        if name in mapped or not sz:
            continue
        i = bisect.bisect_right(ks, val) - 1
        best = None
        for d in {shifts[j][1] for j in (i, i + 1) if 0 <= j < len(shifts)}:
            b0 = val - ta + d
            if b0 < 0 or b0 + sz > len(bm) * 4:
                continue
            same = (bm[b0 // 4:b0 // 4 + sz // 4] == em[(val - ta) // 4:(val - ta) // 4 + sz // 4]).mean()
            if best is None or same > best[1]:
                best = (b0, same)
        if best and best[1] > 0.25:
            mapped[name] = (best[0], sz)
    return sorted((o, s, n) for n, (o, s) in mapped.items()), bw


def rv_base(pcs, bw):
    """the run-time address of the bFLT's first byte: the offset most executed words agree with"""
    rows = []
    for l in open(pcs):
        if l.startswith('#'):
            continue
        p = l.split()
        rows.append((int(p[2], 16), int(p[3], 16), int(p[4])))
    rows.sort(key=lambda r: -r[2])
    where = collections.defaultdict(list)
    for i, w in enumerate(bw.tolist()):
        where[w].append(i * 4)
    votes = collections.Counter()
    for pc, w, n in rows[:3000]:
        hits = where.get(w, ())
        if 0 < len(hits) <= 3:
            for h in hits:
                votes[pc - h] += 1
    return votes.most_common(1)[0][0]


def main():
    what = sys.argv[1]
    if what == 'rv':
        bflt, elf, pcs, out = sys.argv[2:6]
        funcs, bw = rv_funcs(bflt, elf)
        base = rv_base(pcs, bw)
        print(f'{len(funcs)} functions, base {base:08x}')
        with open(out, 'w') as f:
            f.write(f'# base {base:08x}\n')
            for o, s, n in funcs:
                f.write(f'{base + o:08x}\t{base + o + s:08x}\t{n}\n')
    else:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tick', 'gamebench_rv'))
        import hotcode_lib as H
        elf, prof, out = sys.argv[2:5]
        (ta, tb), funcs = H.elf(elf)
        rows = H.read_prof(prof)
        base = H.find_base(rows, ta, tb)
        print(f'{len(funcs)} functions, base {base:+x}')
        with open(out, 'w') as f:
            f.write(f'# base {base:+x}\n')
            for v, s, n in funcs:
                f.write(f'{v + base:08x}\t{v + base + s:08x}\t{n}\n')


if __name__ == '__main__':
    main()


def sx(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def pairs(words, pc0):
    """(index of the auipc, target) of each auipc and the load, store or addi after it that uses its register"""
    out = []
    for i, w in enumerate(words):
        if w & 0x7f != 0x17:
            continue
        rd, hi = (w >> 7) & 31, w & 0xfffff000
        for j in range(i + 1, min(i + 4, len(words))):
            u = words[j]
            op, rs1 = u & 0x7f, (u >> 15) & 31
            if rs1 != rd:
                continue
            if op in (0x13, 0x03, 0x07):
                lo = sx(u >> 20, 12)
            elif op in (0x23, 0x27):
                lo = sx(((u >> 25) << 5) | ((u >> 7) & 31), 12)
            else:
                continue
            out.append((i, j, (pc0 + 4 * i + sx(hi, 32) + lo) & 0xffffffff))
            break
    return out


def rv_data(bflt, elf, base, names):
    """names: symbols, or NAME@HEX for the one at that ELF address. {name: run-time address}: each symbol's address as the image's code computes it (auipc pairs that reach it in the
    ELF, decoded at the same place of the same function in the bFLT; the most votes)"""
    funcs, bw = rv_funcs(bflt, elf)
    e, text, efuncs = elf_funcs(elf)
    ta, to, ts = text[3], text[4], text[5]
    ew = np.frombuffer(e[to:to + ts // 4 * 4], dtype='<u4').astype(np.int64).tolist()
    bwl = bw.tolist()
    want = {n.split('@')[0]: (int(n.split('@')[1], 16) if '@' in n else None) for n in names}
    names = list(want)
    syms = {}
    shoff, = struct.unpack_from('<I', e, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', e, 0x2e)
    secs = [struct.unpack_from('<IIIIIIIIII', e, shoff + i * shentsize) for i in range(shnum)]
    for s in secs:
        if s[1] == 2:
            st = secs[s[6]]
            for at in range(s[4], s[4] + s[5], 16):
                nm, val, sz, info = struct.unpack_from('<IIIB', e, at)
                end = e.index(b'\0', st[4] + nm)
                n = e[st[4] + nm:end].decode()
                if n in names and info & 15 == 1 and want[n] in (None, val):
                    syms.setdefault(n, []).append(val)
    where = {n: o for o, s, n in funcs}
    votes = {n: collections.Counter() for n in names}
    for val, sz, fname in efuncs:
        if fname not in where or not sz:
            continue
        i0, n = (val - ta) // 4, sz // 4
        ep = pairs(ew[i0:i0 + n], val)
        b0 = where[fname] // 4
        bp = {(i, j): t for i, j, t in pairs(bwl[b0:b0 + n], where[fname])}
        for i, j, t in ep:
            for nm in names:
                for sv in syms.get(nm, ()):
                    if sv <= t < sv + 16 and (i, j) in bp:
                        votes[nm][(bp[(i, j)] - (t - sv)) + base] += 1
    return {n: (v.most_common(1)[0] if v else None) for n, v in votes.items()}
