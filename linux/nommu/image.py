"""build/images/NAME (default nommu): the shared image with the flat root (root.sh) as its overlay, the
no-MMU kernel (kernel.sh), and RAM in the device tree from the kernel's address to the GPU's memory
and from 0x88000000 to 0x92000000 (docs/system/nommu.md, "Memory"). Without a build
in build/nommu, linux/prebuilt/nommu: its kernel and the files that differ from linux/prebuilt/root.
   python linux/nommu/image.py [NAME [ROOT [KERNEL]]] [--save-prebuilt]"""
import filecmp, os, shutil, struct, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PREBUILT = os.path.join(ROOT, 'linux', 'prebuilt', 'nommu')
save = '--save-prebuilt' in sys.argv
argv = [a for a in sys.argv[1:] if a != '--save-prebuilt']
built = os.path.join(ROOT, 'build', 'nommu')
own = os.path.exists(os.path.join(built, 'Image')) and os.path.isdir(os.path.join(built, 'flatroot'))
name = argv[0] if len(argv) > 0 else 'nommu'
flat = argv[1] if len(argv) > 1 else os.path.join(built, 'flatroot') if own else os.path.join(PREBUILT, 'root')
kernel = argv[2] if len(argv) > 2 else os.path.join(built, 'Image') if own else os.path.join(PREBUILT, 'Image')
if save and os.path.abspath(flat).startswith(PREBUILT):
    raise SystemExit('--save-prebuilt saves a build (build/nommu): there is none')
# the desktop's icons and fonts are made on this side, never by root.sh: put on a built root (as all.ps1 does)
share = os.path.join(flat, 'usr', 'share')
if not os.path.abspath(flat).startswith(PREBUILT) and os.path.isdir(share):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'make_icons.py')], env=dict(os.environ, ICONS_OUT=share),
                   check=True, stdout=subprocess.DEVNULL)
    if not os.path.exists(os.path.join(share, 'font-ui-12.sef')):
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'make_fonts.py'), share], check=True, stdout=subprocess.DEVNULL)
D = os.path.join(ROOT, 'build', 'images', name)
if os.path.isdir(os.path.join(D, 'overlay')):
    shutil.rmtree(os.path.join(D, 'overlay'))
os.makedirs(D, exist_ok=True)
shutil.copytree(flat, os.path.join(D, 'overlay'), symlinks=True)
shutil.copy(kernel, os.path.join(D, 'Image'))
# Half-Life's engine wants Uplink's data, fetched into this image only (HALFLIFE_DATA=0: none)
if os.path.exists(os.path.join(D, 'overlay', 'usr', 'bin', 'halflife.bin')) and os.environ.get('HALFLIFE_DATA') != '0':
    if subprocess.run([sys.executable, os.path.join(ROOT, 'linux', 'halflife', 'data.py'), os.path.join(D, 'overlay')]).returncode:
        print('Uplink\'s data could not be fetched: the image has Half-Life without it')
r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'private_image.py'), name], cwd=ROOT, capture_output=True, text=True)
print(r.stdout[-600:], r.stderr[-600:])
if r.returncode:
    raise SystemExit('tools/private_image.py %s failed (exit %d): see its output above' % (name, r.returncode))
# RAM from the kernel at 0x80400000 (without an MMU, memory below it is no RAM the kernel may hand
# out) to the GPU's memory at 0x86000000, and above the devices from 0x88000000 to 0x92000000
# (where the state texture ends: kStateHeight in harness/apps/rvc_backend.h)
RAM = [(0x80400000, 0x05c00000), (0x88000000, 0x0a000000)]


def with_ram(dtb):
    """The device tree with the memory node's reg as RAM: the property grows, so every offset of
    the header past it moves."""
    old = struct.pack('>IIII', 0, 0x80000000, 0, 0x06000000)
    value = b''.join(struct.pack('>IIII', 0, at, 0, size) for at, size in RAM)
    assert dtb.count(old) == 1 and dtb[:4] == b'\xd0\x0d\xfe\xed', 'device tree: expected one memory range 0x80000000 + 0x06000000'
    i = dtb.index(old)
    assert struct.unpack('>III', dtb[i - 12:i])[:2] == (3, 16), 'device tree: the range is not a property of 16 bytes'
    grow = len(value) - len(old)
    head = list(struct.unpack('>10I', dtb[:40]))
    head[1] += grow                       # totalsize
    for k in (2, 3, 4):                   # the struct, strings and reserved map's offsets
        if head[k] > i:
            head[k] += grow
    head[9] += grow                       # size_dt_struct
    out = bytearray(dtb[:i - 8]) + struct.pack('>I', len(value)) + dtb[i - 4:i] + value + dtb[i + len(old):]
    out[:40] = struct.pack('>10I', *head)
    return bytes(out)


def with_seed(dtb):
    """The device tree with an rng-seed of 128 bytes in /chosen, which the kernel trusts (RANDOM_TRUST_BOOTLOADER):
    the random pool is ready before /init. Without it nothing credited the pool, and a getrandom() without
    GRND_NONBLOCK waited for ever. The kernel's fast load takes the first 64 bytes and credits nothing; what is
    past them is credited (a seed of 64 left crng_init at 1). The struct block grows by the property, the strings
    block by its name."""
    assert b'rng-seed\0' not in dtb
    head = list(struct.unpack('>10I', dtb[:40]))
    st, ss = head[3], head[8]
    assert head[2] < st and st + ss == head[1], 'device tree: the strings are not last'
    seed = bytes.fromhex(os.environ['RNG_SEED']) if os.environ.get('RNG_SEED') else os.urandom(128)   # (a fixed one to compare kernels)
    node = dtb.index(b'\0\0\0\x01chosen\0', head[2]) + 4
    after = node + (len(b'chosen\0') + 3) // 4 * 4
    prop = struct.pack('>III', 3, len(seed), ss) + seed
    out = bytearray(dtb[:after]) + prop + dtb[after:] + b'rng-seed\0'
    head[1] = len(out)
    head[3] += len(prop)
    head[8] += len(b'rng-seed\0')
    head[9] += len(prop)
    out[:40] = struct.pack('>10I', *head)
    return bytes(out)


TREE_AT = 0x2200000   # the device tree in the boot image (tools/make_linux_image.py)
tree = open(os.path.join(D, 'dts.bin'), 'rb').read()
payload = open(os.path.join(D, 'linux_payload.bin'), 'rb').read()
assert payload[TREE_AT:] == tree, 'linux_payload.bin does not end with dts.bin'
tree = with_seed(with_ram(tree))
open(os.path.join(D, 'dts.bin'), 'wb').write(tree)
open(os.path.join(D, 'linux_payload.bin'), 'wb').write(payload[:TREE_AT] + tree)
# what this machine can neither run nor read leaves the ROM (slim.py)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slim
slim.main(D)
print(name, 'made from', os.path.relpath(flat, ROOT), 'and', os.path.relpath(kernel, ROOT),
      '; memory', ', '.join('0x%08x + 0x%08x' % r for r in RAM))

if save:
    # what the overlay needs over linux/prebuilt/root: save the MMU build there first
    base = os.path.join(ROOT, 'linux', 'prebuilt', 'root')
    shutil.rmtree(PREBUILT, ignore_errors=True)
    os.makedirs(PREBUILT)
    shutil.copyfile(kernel, os.path.join(PREBUILT, 'Image'))
    kept = 0
    for folder, _, names in os.walk(flat):
        for n in names:
            p = os.path.join(folder, n)
            rel = os.path.relpath(p, flat)
            q = os.path.join(base, rel)
            if os.path.exists(q) and filecmp.cmp(p, q, shallow=False):
                continue
            os.makedirs(os.path.dirname(os.path.join(PREBUILT, 'root', rel)), exist_ok=True)
            shutil.copy2(p, os.path.join(PREBUILT, 'root', rel))
            kept += 1
    print('linux/prebuilt/nommu now holds this kernel and the', kept, 'files that differ from linux/prebuilt/root')
