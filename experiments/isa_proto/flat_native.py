"""Every ELF program under a root made the machine's flat program in place (xlat flat, FLAT_FLAG_NATIVE), as
linux/nommu/flat.py makes the RISC-V ones; a program not linked position-independent is reported and left.
   python3 flat_native.py XLAT ROOT"""
import os, struct, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'linux', 'nommu'))
xlat, root = sys.argv[1], sys.argv[2]
BIG = {'quake3.bin', 'halflife.bin', 'quake.bin', 'doom.bin', 'game', 'tdawn.bin', 'ralert.bin', 'classicube.bin', 'nxweb', 'nxide', 'imdemo', 'nes.bin',
       'nxpath', 'tcc', 'nxmicropolis', 'nxphys', 'nxray', 'nxplay', 'nxvideo', 'media_test', 'nxpaint', 'nxview', 'nxfiles', 'openttd.bin'}
done, left, slots, ops = 0, [], 0, 0
for d, _, files in os.walk(root):
    for f in files:
        p = os.path.join(d, f)
        if os.path.islink(p) or not os.path.isfile(p):
            continue
        with open(p, 'rb') as fh:
            head = fh.read(20)
        if head[:4] != b'\x7fELF':
            continue
        if struct.unpack_from('<H', head, 16)[0] != 3 or p.endswith('.o'):
            left.append((p, 'not position-independent'))
            continue
        r = subprocess.run([xlat, 'flat', p, p + '.flt', '0x100000' if f in BIG else '0x40000'], capture_output=True, text=True)
        if r.returncode:
            left.append((p, (r.stderr.strip().splitlines() or ['exit %d' % r.returncode])[-1]))
            continue
        words = r.stdout.split()
        ops += int(words[words.index('ops') - 1]) if 'ops' in words else 0
        slots += int(words[words.index('slots') - 1]) if 'slots' in words else 0
        mode = os.stat(p).st_mode
        os.replace(p + '.flt', p)
        os.chmod(p, mode)
        done += 1
print(f'{done} programs made the machine\'s: {ops} RISC-V instructions in {slots} slots ({ops / max(slots, 1):.2f} a slot)')
for p, why in left:
    print(f'  left: {p}: {why}')
