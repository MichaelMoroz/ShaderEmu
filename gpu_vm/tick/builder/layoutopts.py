"""The local memory's layout (gen.py's rpk, pk, lay): how the registers r[], the write cache's tags wt[] and data wd[]
are kept as indexable arrays. fxc declares every indexable array with four components an element (", 4" in the DXBC),
so a scalar array takes 16 bytes a value: x23's r, wt and wd are 1,360 elements, 21.7 KB a lane.

  rpk  scalar: r[65] (x23); vec: uint4 R4[17], register k is R4[k >> 2][k & 3]
  pk   scalar: wt[WCN + 1], wd[4 * (WCN + 1)] (x23); vec: uint4 T4[NBK] (a bucket's four tags an element) and uint4 D4[WCN + 1]
       (an entry's texel an element); aos: uint4 W4[5 * NBK], a bucket's tags then its four entries' texels side by side
  lay  split: an array each; merged: one array (uint4 elements when any part is vec or aos: a scalar part takes .x)

Every access is rewritten by matching its brackets, so the rest of the tick is unchanged."""

import re

NL = "\n"

def _sub_arrays(src, maps):
    """rewrite NAME[expr] for every name of maps (a function of the rewritten expr); declarations are left to the caller"""
    pat = re.compile(r'(?<![\w.])(' + '|'.join(maps) + r')\[')
    out, i = [], 0
    while True:
        m = pat.search(src, i)
        if not m:
            out.append(src[i:]); break
        out.append(src[i:m.start()])
        j, depth = m.end(), 1
        while depth:
            c = src[j]
            depth += c == '['
            depth -= c == ']'
            j += 1
        inner = _sub_arrays(src[m.end():j - 1], maps)
        out.append(maps[m.group(1)](inner))
        i = j
    return ''.join(out)

def layoutopts(P, src):
    rpk, pk, lay = P.get('rpk', 'scalar'), P.get('pk', 'scalar'), P.get('lay', 'split')
    if rpk == 'scalar' and pk == 'scalar' and lay == 'split': return src
    decl_old = 'static uint r[65];' + NL + 'static uint wt[WCN + 1];' + NL + 'static uint wd[4 * (WCN + 1)];' + NL
    assert decl_old in src, 'layoutopts: no declarations'
    vec = rpk != 'scalar' or pk != 'scalar'
    merged = lay == 'merged'
    # (name, elements, element kind, index map from (array, offset, expr))
    parts = []
    if rpk == 'wide':   # a register a uint4 element, the scalar ops on its .x (no component select: the wide-register ISA's cost)
        parts.append(('r', '65', 'V', lambda A, o, X: '%s[%s(%s)].x' % (A, o, X)))
    else:
        parts.append(('r', '17' if rpk == 'vec' else '65', 'V' if rpk == 'vec' else 'S',
                      (lambda A, o, X: '%s[%s((%s) >> 2)][(%s) & 3]' % (A, o, X, X)) if rpk == 'vec' else None))
    if pk == 'scalar':
        parts.append(('wt', '(WCN + 1)', 'S', None))
        parts.append(('wd', '(4 * (WCN + 1))', 'S', None))
    elif pk == 'vec':
        parts.append(('wt', 'NBK', 'V', lambda A, o, X: '%s[%s((%s) >> 2)][(%s) & 3]' % (A, o, X, X)))
        parts.append(('wd', '(WCN + 1)', 'V', lambda A, o, X: '%s[%s((%s) >> 2)][(%s) & 3]' % (A, o, X, X)))
    else:   # aos: one array for both
        parts.append(('wt', '(5 * NBK)', 'V', lambda A, o, X: '%s[%s5 * ((%s) >> 2)][(%s) & 3]' % (A, o, X, X)))
        parts.append(('wd', '0', 'V', lambda A, o, X: '%s[%s5 * ((%s) >> 4) + 1 + (((%s) >> 2) & 3)][(%s) & 3]' % (A, o, X, X, X)))
    decl = 'static const uint NBK = (WCN + 4) / 4;   // the tags\' elements: buckets of four (the sink\'s too)' + NL
    names = {'r': 'R4' if rpk in ('vec', 'wide') else 'r', 'wt': {'scalar': 'wt', 'vec': 'T4', 'aos': 'W4'}[pk],
             'wd': {'scalar': 'wd', 'vec': 'D4', 'aos': 'W4'}[pk]}
    maps, offs = {}, {}
    if merged:
        A = 'M4' if vec else 'M'
        acc = []   # the sizes before this part
        for n, size, kind, f in parts:
            if n == 'wd' and pk == 'aos': offs[n] = offs['wt']
            else: offs[n] = ' + '.join(acc) if acc else ''
            if size != '0': acc.append(size)
            o = ''
            if offs[n]:
                decl += 'static const uint O_%s = %s;' % (n.upper(), offs[n]) + NL
                o = 'O_%s + ' % n.upper()
            if f: maps[n] = (lambda f, o: lambda X: f(A, o, X))(f, o)
            elif vec: maps[n] = (lambda o: lambda X: '%s[%s(%s)].x' % (A, o, X))(o)
            else: maps[n] = (lambda o: lambda X: '%s[%s(%s)]' % (A, o, X))(o)
        decl += 'static %s %s[%s];   // registers, tags and data in one array (lay=merged)' % ('uint4' if vec else 'uint', A, ' + '.join(acc)) + NL
    else:
        done = set()
        for n, size, kind, f in parts:
            A = names[n]
            if A not in done and size != '0':
                decl += 'static %s %s[%s];' % ('uint4' if kind == 'V' else 'uint', A, size) + NL
                done.add(A)
            maps[n] = (lambda f, A: (lambda X: f(A, '', X)) if f else (lambda X: '%s[%s]' % (A, X)))(f, A)
    if pk != 'scalar':
        wdmap = maps['wd']
        def wd2(X, wdmap=wdmap):
            m = re.match(r'^4 \* ([\w.]+) \+ ([\w.]+)$', X)
            if not m: return wdmap(X)
            e, i = m.groups()
            t = wdmap('4 * ' + e)   # (component 0 of the entry's element)
            return t[:t.rindex('[')] + '[' + i + ']'
        maps['wd'] = wd2
    src = src.replace(decl_old, '@@DECL@@' + NL, 1)
    # hot spots first, as whole elements: a bucket's tags, the clear, the state's output and input
    if pk != 'scalar':
        b_old = 'uint4 bucket(uint b) { uint k = 4 * b; return uint4(wt[k], wt[k + 1], wt[k + 2], wt[k + 3]); }'
        assert b_old in src
        tel = maps['wt']('4 * b')   # element b, component 0: strip the component
        src = src.replace(b_old, 'uint4 bucket(uint b) { return @@TEL@@; }'.replace('@@TEL@@', tel[:tel.rindex('[')]), 1)
        for kw in ('loop', 'unroll'):
            c_old = '[%s] for (k = 0; k <= WCN; k++) wt[k] = NONE;' % kw
            tel = maps['wt']('4 * k')
            src = src.replace(c_old, '[%s] for (k = 0; k < NBK; k++) %s = NONE;' % (kw, tel[:tel.rindex('[')]), 1)
        d_old = '{ uint e = 4 * (t - T_WC); return uint4(wd[e], wd[e + 1], wd[e + 2], wd[e + 3]); }'
        assert d_old in src
        tel = maps['wd']('4 * e')
        src = src.replace(d_old, '{ uint e = t - T_WC; return %s; }' % tel[:tel.rindex('[')], 1)
    if rpk == 'vec':
        o_old = 'return uint4(r[4 * t], r[4 * t + 1], r[4 * t + 2], r[4 * t + 3]);'
        assert o_old in src
        tel = maps['r']('4 * t')
        src = src.replace(o_old, 'return %s;' % tel[:tel.rindex('[')], 1)
        i_old = 'r[4 * k] = s.x; r[4 * k + 1] = s.y; r[4 * k + 2] = s.z; r[4 * k + 3] = s.w;'
        assert i_old in src
        tel = maps['r']('4 * k')
        src = src.replace(i_old, '%s = s;' % tel[:tel.rindex('[')], 1)
    src = _sub_arrays(src, maps)
    return src.replace('@@DECL@@' + NL, decl, 1)
