#!/bin/bash
# The port's sources into the kernel tree: arch/gpuvm copied over (what is gone here goes there too), and
# patches/*.patch applied once each. Run in WSL. KDIR: the tree (default ~/gpu_vm/linux, Linux 5.17.11 committed
# pristine in git, so `git -C $KDIR diff` is the port's change to generic files).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KDIR="${KDIR:-$HOME/gpu_vm/linux}"

[ -f "$KDIR/Makefile" ] || { echo "sync.sh: no kernel tree at $KDIR" >&2; exit 1; }
v=$(make -s -C "$KDIR" kernelversion)
[ "$v" = 5.17.11 ] || { echo "sync.sh: $KDIR is $v, not 5.17.11" >&2; exit 1; }

rsync -a --delete --checksum "$HERE/arch/gpuvm/" "$KDIR/arch/gpuvm/"

for p in "$HERE"/patches/*.patch; do
    if git -C "$KDIR" apply --reverse --check "$p" 2>/dev/null; then
        continue                                   # applied already
    fi
    git -C "$KDIR" apply "$p"
    echo "sync.sh: applied $(basename "$p")"
done
