#!/usr/bin/env bash
# gpu_vm's root on the ROM with the machine's programs (gpu_vm/userland/apps/README.md): rootfs.sh's tree (busybox,
# /etc, Lua, QuickJS, the tests), then what build.sh made (build/gvm_apps/root), then the RISC-V image's data that no
# gpu_vm build put there (linux/prebuilt/root and linux/prebuilt/nommu/root: the games' files, pictures, fonts; never
# a RISC-V program). A romfs image (mkromfs.py), the romroot kernel (romroot.sh) and a device tree with the ROM's
# size and RAM_MB of RAM. In WSL:
#   bash gpu_vm/userland/apps/image.sh            -> build/gvm_apps/img: rootfs.romfs, gvm.dtb, vmlinux, busybox
# DATA=0 leaves the RISC-V image's data out (a small ROM for the tests). RAM_MB (default 192: gvm_cpu --ram).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
UL=$(cd "$HERE/.." && pwd)
ROOT=$(cd "$UL/../.." && pwd)
R=${R:-$HOME/gpu_vm/rootfs-build}
APPS=${APPS:-$ROOT/build/gvm_apps/root}
W=${W:-$ROOT/build/gvm_apps/img}
K=${K:-$ROOT/build/gvm_apps/kernel}
KOUT=${KOUT:-$HOME/gpu_vm/linux-build-apps}
RAM_MB=${RAM_MB:-192}
[ -f "$R/initramfs.list" ] || { echo "image.sh: no $R/initramfs.list (rootfs.sh)" >&2; exit 1; }
[ -f "$K/vmlinux" ] || { echo "image.sh: no $K/vmlinux (romroot.sh)" >&2; exit 1; }
mkdir -p "$W"
L=$W/rootfs.list
python3 - "$R/initramfs.list" "$HERE/rcS" "$APPS" "${DATA:-1}" "$ROOT/linux/prebuilt/nommu/root" "$ROOT/linux/prebuilt/root" > "$L" <<'EOF'
import os, sys
base, rcs, apps, data = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == '1'
extra = sys.argv[5:]
lines, have, dirs = [], set(), set()
# (a program of both, rootfs.sh's and build.sh's, is build.sh's: QuickJS's newer qjs)
ours = set()
for dp, dn, fn in os.walk(apps):
    rel = os.path.relpath(dp, apps).replace(os.sep, '/')
    for n in fn: ours.add(('' if rel == '.' else '/' + rel) + '/' + n)
for line in open(base):
    f = line.split()
    if not f: continue
    if f[0] in ('file', 'slink') and f[1] in ours: continue
    if f[0] == 'file' and f[1] == '/etc/init.d/rcS': line = 'file /etc/init.d/rcS %s 0755 0 0' % rcs
    lines.append(line.rstrip('\n'))
    if f[0] in ('file', 'slink', 'nod', 'dir'): have.add(f[1])
    if f[0] == 'dir': dirs.add(f[1])
def add_dir(p):
    parts = [x for x in p.split('/') if x]
    for i in range(1, len(parts) + 1):
        d = '/' + '/'.join(parts[:i])
        if d not in dirs and d not in have:
            lines.append('dir %s 0755 0 0' % d); dirs.add(d); have.add(d)
def is_program(path):
    with open(path, 'rb') as f: head = f.read(4)
    return head in (b'bFLT', b'\x7fELF')
def walk(top, programs):
    for dp, dn, fn in os.walk(top):
        dn.sort()
        rel = os.path.relpath(dp, top).replace(os.sep, '/')
        rel = '' if rel == '.' else '/' + rel
        for n in sorted(fn):
            src = os.path.join(dp, n); p = rel + '/' + n
            if p in have or n.endswith('.gdb') or n.endswith('.romlink'): continue
            sep = chr(9) if ' ' in p or ' ' in src else ' '
            if os.path.islink(src):
                add_dir(rel or '/'); lines.append(sep.join(('slink', p, os.readlink(src), '0777', '0', '0'))); have.add(p); continue
            if not programs and is_program(src): continue
            add_dir(rel or '/')
            mode = '0755' if os.access(src, os.X_OK) or is_program(src) else '0644'
            lines.append(sep.join(('file', p, src, mode, '0', '0'))); have.add(p)
add_dir('/persistence')
walk(apps, True)
if data:
    for top in extra: walk(top, False)
print('\n'.join(lines))
EOF
python3 "$UL/mkromfs.py" "$L" "$W/rootfs.romfs"
MIB=$(( ($(stat -c %s "$W/rootfs.romfs") + 1048575) / 1048576 ))
sed -e "s|bootargs = \"console=hvc0\";|bootargs = \"console=hvc0 root=mtd:root rootfstype=romfs ro init=/init phram.phram=root,0x40000000,${MIB}Mi\";|" \
    -e "s|reg = <0x02000000 0x04000000>;|reg = <0x02000000 $(printf '0x%08x' $((RAM_MB << 20)))>;|" \
    "$ROOT/gpu_vm/linux/arch/gpuvm/boot/dts/gvm.dts" > "$W/gvm.dts"
"$KOUT/scripts/dtc/dtc" -q -I dts -O dtb -o "$W/gvm.dtb" "$W/gvm.dts"
cp "$K/vmlinux" "$W/vmlinux"; cp "$K/busybox" "$W/busybox"
echo "$RAM_MB" > "$W/ram_mb"
ls -l "$W/rootfs.romfs"
echo "image.sh: $W ($MIB MB of ROM, $RAM_MB MB of RAM)"
