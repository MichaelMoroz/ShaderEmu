import json,sys
for l in open('boot.jsonl'):
    r=json.loads(l); b=r.get('boot')
    if not b: print(r['tag'],'no marks',r['done'],r['marks']); continue
    k=b['kinds']; print(r['tag'],'prompt ms',b['wall_ms'],'passes',b['passes'],'c0 M',round(b['core0']/1e6,2),'a pass',b['core0_a_pass'],'full%',k.get('write full',{}).get('share',0),'idle ms',k.get('idle',{}).get('ms',0),'oops',r['oops'],'done',r['done'])
