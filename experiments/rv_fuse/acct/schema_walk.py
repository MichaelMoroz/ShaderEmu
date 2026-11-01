#!/usr/bin/env python3
"""Fields of a trace's protobuf, walked by ngfx-trace schema: python schema_walk.py TRACE PATH [DEPTH] [grep]"""
import json, os, re, subprocess, sys
N = os.path.expandvars(r'%USERPROFILE%\.cargo\bin\ngfx-trace.exe')
tr, root = sys.argv[1], sys.argv[2]; depth = int(sys.argv[3]) if len(sys.argv) > 3 else 2; pat = sys.argv[4] if len(sys.argv) > 4 else ''
def walk(path, d):
    p = subprocess.run([N, 'schema', tr, path, '--compact'], capture_output=True, text=True)
    if p.returncode: return
    for f in json.loads(p.stdout).get('fields', []):
        if not f['present']: continue
        sub = path + '.' + f['name'] if path else f['name']
        if not pat or re.search(pat, f['name'], re.I): print(sub, f['kind'], f.get('item_count'), flush=True)
        if d > 0 and f['kind'].startswith(('message', 'list<message')):
            walk(sub + ('.0' if f['kind'].startswith('list') else ''), d - 1)
walk(root, depth)
