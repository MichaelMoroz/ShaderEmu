"""Parses a DXIL disassembly, finds the tick loop and prints the cheapest path one iteration takes
for a given opcode (the path through the first `switch` whose case equals that opcode)."""
import re, sys, heapq, collections

ll = open(sys.argv[1], encoding='utf-8', errors='replace').read().split('\n')
opcode = int(sys.argv[2]) if len(sys.argv) > 2 else 19
blocks, order, cur = collections.OrderedDict(), [], None
in_func = False
for line in ll:
    if line.startswith('define '):
        in_func = True
        cur = 'entry'; blocks[cur] = []
        continue
    if not in_func: continue
    if line.startswith('}'): break
    m = re.match(r'^; <label>:(\d+)', line)
    if m:
        cur = m.group(1); blocks[cur] = []
        continue
    s = line.strip()
    if s and not s.startswith(';'):
        blocks[cur].append(s)

succ, kind = {}, {}
for b, ins in blocks.items():
    if not ins: succ[b] = []; continue
    t = ins[-1]
    j = len(ins) - 1
    # a switch terminator spans several lines
    while j >= 0 and not re.match(r'^(br|switch|ret|unreachable)\b', ins[j]): j -= 1
    term = ' '.join(ins[j:]) if j >= 0 else t
    labels = re.findall(r'label %(\d+)', term)
    succ[b] = labels
    kind[b] = term.split()[0]
    blocks[b] = ins[:j] + [term] if j >= 0 else ins

def cost(b):   # instructions that do work (phis are register moves at best)
    return sum(1 for i in blocks[b] if ' = phi ' not in i)

# loop header: target of a back edge with the most predecessors among blocks that dominate nothing known; approximate by
# the block that is a successor of a later block and has the largest forward reach.
index = {b: i for i, b in enumerate(blocks)}
back = collections.Counter()
for b, ss in succ.items():
    for s in ss:
        if s in index and index[s] <= index[b]: back[s] += 1
print('blocks:', len(blocks), ' back-edge targets:', [(h, n) for h, n in back.most_common(6)])

def find_switch(val):
    for b, ins in blocks.items():
        if kind.get(b) == 'switch':
            m = re.search(r'i32 %d, label %%(\d+)' % val, ins[-1])
            if m and len(re.findall(r'i32 -?\d+, label', ins[-1])) >= 6: return b, m.group(1)
    return None, None

sw, case = find_switch(opcode)
print('dispatch switch block', sw, '-> case block', case)

def shortest(src, dst_set, banned=()):
    dist, prev = {src: cost(src)}, {}
    pq = [(dist[src], src)]
    while pq:
        d, b = heapq.heappop(pq)
        if b in dst_set and b != src:
            path = [b]
            while path[-1] != src: path.append(prev[path[-1]])
            return d, path[::-1]
        if d > dist.get(b, 1e18): continue
        for s in succ.get(b, []):
            if s in banned or s not in blocks: continue
            nd = d + cost(s)
            if nd < dist.get(s, 1e18):
                dist[s], prev[s] = nd, b; heapq.heappush(pq, (nd, s))
    return None, []

# header = the back-edge target from which the dispatch switch is reachable and that is reachable from the case block
best = None
for h, _ in back.most_common(12):
    d1, p1 = shortest(h, {sw})
    d2, p2 = shortest(case, {h})
    if d1 is not None and d2 is not None:
        tot = d1 + d2
        if best is None or tot < best[0]: best = (tot, h, p1, p2)
tot, h, p1, p2 = best
path = p1 + p2[:-1]
print('loop header %s; one iteration for opcode %d: %d blocks, %d instructions (excluding phis)' % (h, opcode, len(path), sum(cost(b) for b in path)))
ops = collections.Counter()
for b in path:
    for i in blocks[b]:
        if ' = phi ' in i: ops['phi'] += 1; continue
        m = re.match(r'(?:%\S+ = )?(\w+)', i)
        op = m.group(1)
        if op == 'call':
            m2 = re.search(r'@dx\.op\.(\w+)', i); op = 'call ' + (m2.group(1) if m2 else '?')
        ops[op] += 1
print(', '.join('%s %d' % kv for kv in ops.most_common()))
print('per block (instructions + phis): ' + ' '.join('%s:%d+%d' % (b, cost(b), len(blocks[b]) - cost(b)) for b in path))
if len(sys.argv) > 3:
    for b in path:
        print('--- block', b, '(%d)' % cost(b))
        for i in blocks[b]:
            if ' = phi ' not in i: print('   ', i[:150])
