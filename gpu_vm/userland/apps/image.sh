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
# the RISC-V image's data out of its ROM (romfs_extract.py of the main checkout's build/images/nommu/rootfs.bin):
# terminal descriptions, file's magic, QuickJS's examples, the desktop's media; last, under everything else
RVDATA=${RVDATA:-$HOME/gpu_vm/riscv-rom-data}
[ -f "$R/initramfs.list" ] || { echo "image.sh: no $R/initramfs.list (rootfs.sh)" >&2; exit 1; }
[ -f "$K/vmlinux" ] || { echo "image.sh: no $K/vmlinux (romroot.sh)" >&2; exit 1; }
mkdir -p "$W"
L=$W/rootfs.list
# the desktop as the RISC-V image starts with it (tools/make_linux_image.py's desktop_shortcuts: a shortcut for every
# program of the Start menu, the places; nxfiles --desktop leaves at once without /root/Desktop), from the apps' menus
rm -rf "$W/desktop" "$W/icons"; mkdir -p "$W/desktop" "$W/icons"
# the references test_all.py's outputs are compared with (gvmcmp), in /usr/share/gvm-refs
REFS=${REFS:-$ROOT/build/gvm_apps/refs}
mkdir -p "$W/desktop/usr/share/gvm-refs"
[ -d "$REFS" ] && find "$REFS" -maxdepth 1 -type f -exec cp {} "$W/desktop/usr/share/gvm-refs/" \;
python3 - "$ROOT/tools" "$APPS" "$W/desktop" <<'PY'
import os, sys, types
sys.modules.setdefault('PIL', types.SimpleNamespace(Image=None))   # (the image builder reads PNGs; not here)
sys.path.insert(0, sys.argv[1])
import make_linux_image as m
for data, folder, name in m.desktop_shortcuts(sys.argv[2]):
    d = os.path.join(sys.argv[3], folder.lstrip('/'))
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, name), 'wb').write(data)
PY
python3 - "$R/initramfs.list" "$HERE/rcS" "$APPS" "${DATA:-1}" "$W/desktop" "$W/icons" "$ROOT/linux/prebuilt/nommu/root" "$ROOT/linux/prebuilt/root"     $( [ -d "$RVDATA" ] && echo "$RVDATA" ) > "$L" <<'EOF'
import os, sys
base, rcs, apps, data, desk, iconized = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4] == '1', sys.argv[5], sys.argv[6]
extra = sys.argv[7:]
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
# a program with its icon at its end, as the RISC-V image has it (make_linux_image.py's with_icon: 32 x 32 then 16 x 16
# pixels of three bytes from the sheets' cell of its name, then the mark), in a copy under $W/icons
ICON_MARK = b'NXICON1' + bytes(1)
def sheet(path):
    d = open(path, 'rb').read()
    parts, at = [], 0
    while len(parts) < 4:
        while d[at:at + 1].isspace(): at += 1
        e = at
        while not d[e:e + 1].isspace(): e += 1
        parts.append(d[at:e]); at = e
    return int(parts[1]), d[at + 1:]
icon_share = next((os.path.join(t, 'usr', 'share') for t in [apps] + extra if os.path.exists(os.path.join(t, 'usr', 'share', 'icons.idx'))), None)
cells = dict(l.split() for l in open(os.path.join(icon_share, 'icons.idx'))) if icon_share else {}
sheets = {z: sheet(os.path.join(icon_share, 'icons%d.ppm' % z)) for z in (32, 16)} if icon_share else {}
def with_icon(src, name):
    body = open(src, 'rb').read()
    if name not in cells or body.endswith(ICON_MARK):
        return src
    cell, out = int(cells[name]), b''
    for z in (32, 16):
        w, px = sheets[z]
        x0, y0 = cell % 16 * z, cell // 16 * z
        for y in range(y0, y0 + z):
            out += px[(y * w + x0) * 3:(y * w + x0 + z) * 3]
    dst = os.path.join(iconized, src.lstrip('/'))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, 'wb').write(body + out + ICON_MARK)
    os.chmod(dst, 0o755)
    return dst
def walk(top, programs):
    for dp, dn, fn in os.walk(top):
        dn.sort()
        rel = os.path.relpath(dp, top).replace(os.sep, '/')
        rel = '' if rel == '.' else '/' + rel
        for n in sorted(fn):
            src = os.path.join(dp, n); p = rel + '/' + n
            if n.endswith('.romlink'):   # NAME.romlink: NAME, a link to the path it names (make_linux_image.py's hard link)
                q = p[:-len('.romlink')]
                if q not in have:
                    add_dir(rel or '/'); lines.append((chr(9) if ' ' in q else ' ').join(('slink', q, open(src).read().strip(), '0777', '0', '0'))); have.add(q)
                continue
            if p in have or n.endswith('.gdb'): continue
            sep = chr(9) if ' ' in p or ' ' in src else ' '
            if programs and not os.path.islink(src) and is_program(src):
                src = with_icon(src, n)
            if os.path.islink(src):
                add_dir(rel or '/'); lines.append(sep.join(('slink', p, os.readlink(src), '0777', '0', '0'))); have.add(p); continue
            if not programs and is_program(src): continue
            add_dir(rel or '/')
            mode = '0755' if os.access(src, os.X_OK) or is_program(src) else '0644'
            lines.append(sep.join(('file', p, src, mode, '0', '0'))); have.add(p)
add_dir('/persistence')
walk(apps, True)
walk(desk, True)
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
