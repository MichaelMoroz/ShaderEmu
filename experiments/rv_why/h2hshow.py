import json, os, sys
for l in open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'h2h.jsonl')):
    r = json.loads(l); w = (r.get('boot') if r['load'] == 'boot' else r.get('work')) or {}
    print(r['machine'], r['load'], r.get('tag'), 'wall', w.get('wall_ms'), 'passes', w.get('passes'), 'a pass', w.get('core0_a_pass'), {k: v['share'] for k, v in w.get('why', {}).items()}, 'done', r['done'], 'oops', r['oops'], [p[-34:] for p in r['proof'][-1:]])
