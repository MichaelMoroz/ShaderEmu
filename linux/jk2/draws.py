"""Calls in the game whose arguments draw two or more random numbers (docs/games/jk2.md, "Is it the same game").
C++ leaves the order of a call's arguments open: GCC evaluates them last to first for x86 and first to last for
RISC-V, so f( Q_flrand(), Q_flrand() ) gave the build computer and the machine two different games (the gun racks'
blasters, at the level's first frame). The patch draws such arguments into variables before the call, last first,
as the x86 order (and the 2002 game's, MSVC's) had them.
   python3 linux/jk2/draws.py TREE        prints each such call: file:line: the call
Exit 0 when there is none left."""
import os, re, sys

DRAW = re.compile(r'\b(Q_flrand|Q_irand|crandom|random|rand|Q_rand|flrand|irand)\s*\(')


def strip(text):
    """Comments, strings and characters blanked to spaces, the lines kept."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + ' ' * (j - i - 1) + c); i = j + 1
        else:
            out.append(c); i += 1
    return ''.join(out)


def calls(text):
    """(offset of the name, name, [arguments as text]) of every call, nested ones too."""
    for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', text):
        name = m.group(1)
        if name in ('if', 'while', 'for', 'switch', 'return', 'sizeof', 'defined'):
            continue
        depth, args, start = 0, [], m.end()
        i = m.end()
        while i < len(text):
            c = text[i]
            if c in '([{':
                depth += 1
            elif c in ')]}':
                if depth == 0:
                    args.append(text[start:i]); break
                depth -= 1
            elif c == ',' and depth == 0:
                args.append(text[start:i]); start = i + 1
            i += 1
        yield m.start(), name, args


def main():
    root = sys.argv[1]
    found = 0
    for d in ('codeJK2/game', 'codeJK2/cgame'):
        for name in sorted(os.listdir(os.path.join(root, d))):
            if not name.endswith('.cpp'):
                continue
            raw = open(os.path.join(root, d, name), encoding='latin-1').read()
            text = strip(raw)
            for at, fn, args in calls(text):
                if sum(1 for a in args if DRAW.search(a)) >= 2 and not DRAW.match(fn + '('):
                    line = text.count('\n', 0, at) + 1
                    print('%s/%s:%d: %s(%s)' % (d, name, line, fn, ','.join(' '.join(a.split()) for a in args)[:200]))
                    found += 1
    print('%d calls' % found, file=sys.stderr)
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
