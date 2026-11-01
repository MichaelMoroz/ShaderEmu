import json, re, sys
for l in sys.stdin:
    if not l.startswith('{'):
        if 'NOT DONE' in l or 'error' in l.lower() or 'failed' in l.lower(): print(l[:400].rstrip())
        continue
    r = json.loads(l)
    g = next((s for s in r.get('stats', []) if 'gpu per frame' in s), '')
    print(r['load'], r['tick'], r['tag'], 'wall', r.get('wall_ms'), 'passes', r.get('passes'), 'a pass', r.get('a_pass'), 'full', r.get('why', {}).get('full', {}).get('share'), 'done', r['done'], 'host', r['host_s'], 'proof', [p[-16:] for p in r['proof']], 'oops', r['oops'], g[-60:])
