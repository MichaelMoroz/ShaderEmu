"""Which instructions a guest runs one after the other, from --pc-log samples and the code itself.

    python tools/pc_ngrams.py PC.LOG CODE.BIN BASE [--top 20]

CODE.BIN is the code as it sits in memory and BASE the address of its first byte in the pc's
terms (programs/bin/raytrace.bin 0x80000000; linux/prebuilt/Image 0xc0000000). Samples outside
it are left out and their share is printed.

How often an instruction ran is its samples' weight (see pc_hot.py for how to sample). A
sequence is counted where the code runs straight through it: no instruction but the last may
be a branch or a jump, and it ran as often as its least-run member.
"""
import collections, sys
import numpy as np

LOADS = ['lb', 'lh', 'lw', 'ld?', 'lbu', 'lhu', '?', '?']
STORES = ['sb', 'sh', 'sw', '?', '?', '?', '?', '?']
BRANCH = ['beq', 'bne', '?', '?', 'blt', 'bge', 'bltu', 'bgeu']
OPIMM = ['addi', 'slli', 'slti', 'sltiu', 'xori', 'srli', 'ori', 'andi']
OP = ['add', 'sll', 'slt', 'sltu', 'xor', 'srl', 'or', 'and']
MUL = ['mul', 'mulh', 'mulhsu', 'mulhu', 'div', 'divu', 'rem', 'remu']
AMO = {0: 'amoadd', 1: 'amoswap', 2: 'lr', 3: 'sc', 4: 'amoxor', 8: 'amoor', 12: 'amoand'}


def decode(w):
    """(mnemonic, class). Classes: alu, load, store, branch, jump, mul, other."""
    opc, f3, f7 = w & 0x7f, (w >> 12) & 7, w >> 25
    if opc == 0x13:
        if f3 == 5 and f7 == 0x20:
            return 'srai', 'alu'
        if w == 0x13:
            return 'nop', 'alu'
        if f3 == 0 and ((w >> 15) & 31) == 0:
            return 'li', 'alu'
        if f3 == 0 and (w >> 20) == 0:
            return 'mv', 'alu'
        return OPIMM[f3], 'alu'
    if opc == 0x33:
        if f7 == 1:
            return MUL[f3], 'mul'
        if f7 == 0x20:
            return ('sub' if f3 == 0 else 'sra'), 'alu'
        return OP[f3], 'alu'
    if opc == 0x03:
        return LOADS[f3], 'load'
    if opc == 0x23:
        return STORES[f3], 'store'
    if opc == 0x63:
        return BRANCH[f3], 'branch'
    if opc == 0x37:
        return 'lui', 'alu'
    if opc == 0x17:
        return 'auipc', 'alu'
    if opc == 0x6f:
        return ('j' if ((w >> 7) & 31) == 0 else 'jal'), 'jump'
    if opc == 0x67:
        return ('ret' if w == 0x8067 else 'jalr'), 'jump'
    if opc == 0x2f:
        return AMO.get(w >> 27, 'amo?'), 'other'
    if opc == 0x73:
        return ('csr' if f3 else 'system'), 'other'
    if opc == 0x0f:
        return 'fence', 'other'
    return '?', 'other'


def main():
    args = sys.argv[1:]
    top = 20
    if '--top' in args:
        i = args.index('--top')
        top = int(args[i + 1])
        del args[i:i + 2]
    samples = np.fromfile(args[0], dtype=np.uint32).reshape(-1, 2)
    code = np.fromfile(args[1], dtype=np.uint8)
    code = code[:len(code) // 4 * 4].view(np.uint32)
    base = int(args[2], 0)

    everything = float(samples[:, 1].sum())
    index = (samples[:, 0].astype(np.int64) - base) // 4
    inside = (index >= 0) & (index < len(code)) & ((samples[:, 0] & 3) == 0)
    weight = np.bincount(index[inside], weights=samples[inside, 1].astype(np.float64), minlength=len(code))
    total = weight.sum()
    print('%s: %.0f instructions, %.1f%% of them in %s' % (args[0], everything, 100 * total / everything, args[1]))

    hot = np.nonzero(weight)[0]
    names, classes = {}, {}
    for i in range(max(0, hot.min() - 8), min(len(code), hot.max() + 9)):
        names[i], classes[i] = decode(int(code[i]))

    mix, by_name = collections.Counter(), collections.Counter()
    for i in hot:
        mix[classes[i]] += weight[i]
        by_name[names[i]] += weight[i]
    print('  mix: ' + ', '.join('%s %.1f%%' % (k, 100 * v / total) for k, v in mix.most_common()))
    print('  commonest: ' + ', '.join('%s %.1f%%' % (k, 100 * v / total) for k, v in by_name.most_common(12)))
    control = mix['branch'] + mix['jump']
    print('  instructions per branch or jump run: %.1f' % (total / control if control else 0))

    def ran(i):
        return weight[i] if 0 <= i < len(code) else 0.0

    def straight(i, n):
        """Whether code runs straight through i..i+n-1, and how often it did."""
        for k in range(n - 1):
            if classes.get(i + k) in ('branch', 'jump', None) or names[i + k] == 'system':
                return 0.0
        if i + n - 1 not in classes:
            return 0.0
        return min(ran(i + k) for k in range(n))

    grams = {}
    for n in (2, 3, 4):
        count = collections.Counter()
        for i in hot:
            c = straight(int(i), n)
            if c:
                count[tuple(names[int(i) + k] for k in range(n))] += c
        grams[n] = count
        print('  sequences of %d (share of all instructions that start one):' % n)
        for seq, c in count.most_common(top):
            print('    %5.2f%%  %s' % (100 * c / total, ' '.join(seq)))

    # How many trips through the interpreter's loop a set of wide instructions saves: the code is
    # walked in order and the longest sequence in the set is taken wherever one starts.
    print('  with the K commonest sequences made one instruction each (longest match first):')
    for longest in (2, 3, 4):
        for k in (8, 16, 32, 64, 256):
            chosen = set()
            for n in range(2, longest + 1):
                chosen.update(seq for seq, _ in grams[n].most_common(k))
            saved, i, end = 0.0, int(hot.min()), int(hot.max())
            while i <= end:
                step = 1
                if weight[i]:
                    for n in range(longest, 1, -1):
                        c = straight(i, n)
                        if c and tuple(names[i + j] for j in range(n)) in chosen:
                            saved += c * (n - 1)
                            step = n
                            break
                i += step
            print('    up to %d long, %3d of each length: %4.1f%% fewer trips' % (longest, k, 100 * saved / total))
    print()


if __name__ == '__main__':
    main()
