import re,sys
for f in sys.argv[1:]:
    t=open(f,encoding='latin-1').read(); T=S=0; p={}
    for m in re.finditer(r'openttd: play (scroll|zoomed out|windows): (\d+) pictures in ([\d.]+) s, ([\d.]+) a second, (\d+) ticks',t):
        T+=int(m[5]); S+=float(m[3]); p[m[1]]=m[4]
    print(f"{f.split('/')[-1]:14s} ticks/s {T/S:.2f}  {p}")
