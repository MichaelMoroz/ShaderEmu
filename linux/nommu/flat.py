"""Every ELF program under a root made a flat program in place (elf2bflt.py); a program not linked
position-independent is reported and left as it is.   python3 flat.py ROOT"""
import os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
BIG = {'quake3.bin', 'jk2.bin', 'halflife.bin', 'quake.bin', 'doom.bin', 'game', 'tdawn.bin', 'ralert.bin', 'classicube.bin', 'nxweb', 'nxide', 'imdemo', 'nes.bin',
       'nxpath', 'tcc', 'nxmicropolis', 'nxphys', 'nxray', 'nxplay', 'nxvideo', 'media_test', 'nxpaint', 'nxview', 'nxfiles', 'openttd.bin', 'aoe.bin'}
root = sys.argv[1]
done, left = 0, []
for d, _, files in os.walk(root):
    for f in files:
        p = os.path.join(d, f)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        with open(p, 'rb') as fh:
            head = fh.read(20)
        if head[:4] != b'\x7fELF':
            continue
        e_type = struct.unpack_from('<H', head, 16)[0]
        if e_type != 3:
            left.append((p, e_type))
            continue
        stack = '0x100000' if f in BIG else '0x40000'
        r = subprocess.run([sys.executable, os.path.join(HERE, 'elf2bflt.py'), p, p + '.flt', stack], capture_output=True, text=True)
        if r.returncode:
            left.append((p, r.stderr.strip().splitlines()[-1]))
            continue
        for line in r.stdout.splitlines():
            if line.startswith('WARNING'):
                print(line)
        mode = os.stat(p).st_mode
        os.replace(p + '.flt', p)
        os.chmod(p, mode)
        done += 1
print(f'{done} programs made flat')
for p, why in left:
    print(f'  left: {p}: {why}')
