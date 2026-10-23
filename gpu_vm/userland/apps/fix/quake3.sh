#!/usr/bin/env bash
# Quake III's data on gpu_vm (run by build.sh on its copy, R): the baked level (bake.py) is the same bytes for both
# machines, and bake.py wants numpy; where this computer's python has none, the RISC-V image's baked files
# (linux/prebuilt/root/usr/share/quake3-pak0.pk3, quake3-se.dat) are taken instead.
set -euo pipefail
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
P=$REPO/linux/prebuilt/root/usr/share
f=$R/linux/quake3/build.sh
grep -q '^    python3 "\$HERE/bake.py"' "$f" || { echo "fix/quake3.sh: bake.py's line not found"; exit 1; }
sed -i "s|^    python3 \"\\\$HERE/bake.py\"|    python3 -c 'import numpy' 2>/dev/null \|\| { cp '$P/quake3-pak0.pk3' \"\$DATA/pak0.pk3\"; cp '$P/quake3-se.dat' \"\$DATA/se.dat\"; }; python3 -c 'import numpy' 2>/dev/null \&\& python3 \"\$HERE/bake.py\"|" "$f"
grep -q "import numpy" "$f" || { echo "fix/quake3.sh: not changed"; exit 1; }
# The three modules' partial links keep each function's section (.text alone merged), so that the program's link can
# lay their functions out (GVM_ORDER_FILE: gpu_vm/userland/bin/gpuvm-linux-cc)
grep -q '^    .text : { \*(.text .text.\*) }$' "$f" && sed -i 's/^    .text : { \*(.text .text.\*) }$/    .text : { *(.text) }/' "$f"
grep -q '^    .text : { \*(.text) }$' "$f" || { echo "fix/quake3.sh: the modules' .text line not found"; exit 1; }
