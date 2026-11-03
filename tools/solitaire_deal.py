"""nxsolitaire's deal of a number, as deal() in linux/apps/nxsolitaire.c makes it, for writing a test:

    python tools/solitaire_deal.py 7          the seven columns and the stock of deal 7
    python tools/solitaire_deal.py --find     a deal with what tools/test_all.py's solitaire case plays
"""
import sys

NAMES = 'A23456789TJQK'


def name(c):
    return NAMES[c % 13] + 'cdhs'[c // 13]


def deal(number):
    pack, s = list(range(52)), number
    for i in range(51, 0, -1):
        s = (s * 1664525 + 1013904223) & 0xffffffff
        j = (s >> 8) % (i + 1)
        pack[i], pack[j] = pack[j], pack[i]
    cols, at = [[] for _ in range(7)], 0
    for k in range(7):
        for i in range(k, 7):
            cols[i].append(pack[at])
            at += 1
    return cols, pack[at:]   # the stock: its last is its top


def fits(card, onto):
    red = lambda c: c // 13 in (1, 2)
    return red(card) != red(onto) and onto % 13 == card % 13 + 1


def find():
    """A deal whose first stock card is no ace, with an ace on a column's top, and two moves of a column's
    top onto another's that do not share a card."""
    for number in range(1, 100000):
        cols, stock = deal(number)
        tops = [c[-1] for c in cols]
        if not any(t % 13 == 0 for t in tops) or stock[-1] % 13 == 0:
            continue
        moves = [(a, b) for a in range(7) for b in range(7) if a != b and fits(tops[a], tops[b])]
        pairs = [(m, n) for m in moves for n in moves if len({m[0], m[1], n[0], n[1]}) == 4]
        if pairs:
            return number, pairs[0]


if __name__ == '__main__':
    if sys.argv[1:] == ['--find']:
        print(find())
    else:
        cols, stock = deal(int(sys.argv[1]))
        for k, c in enumerate(cols):
            print('t%d: %s' % (k + 1, ' '.join(name(x) for x in c)))
        print('stock (top last): %s' % ' '.join(name(x) for x in stock))
