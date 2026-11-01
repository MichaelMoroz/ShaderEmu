import json, sys
for l in sys.stdin:
    if not l.startswith('{'):
        if 'NOT DONE' in l: print(l[:300].rstrip())
        continue
    r = json.loads(l); w = r.get('work') or r.get('boot') or {}
    if r['load'] == 'boot': w = r.get('boot') or {}
    print(r['machine'], r['load'], r.get('tag'), 'wall', w.get('wall_ms'), 'passes', w.get('passes'), 'a pass', w.get('core0_a_pass'), {k: v['share'] for k, v in w.get('why', {}).items()}, 'done', r['done'], 'oops', r['oops'], [p[-40:] for p in r['proof'][-1:]])
