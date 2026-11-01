#!/usr/bin/env bash
# The private home's Red Alert, flat, into a root: flat_ra.sh ROOT
set -euo pipefail
ROOT=$1
H=$HOME/agents/play
export PATH=$HOME/shaderemu-linux/toolchain/bin:$PATH
T=$H/flat_tmp; rm -rf "$T"; mkdir -p "$T/usr/lib/ralert"
cp "$H/nm/repo/build/images/linux/root/usr/lib/ralert/game" "$T/usr/lib/ralert/game"
riscv32-linux-strip "$T/usr/lib/ralert/game" || true
python3 linux/nommu/flat.py "$T" > /dev/null
cp "$T/usr/lib/ralert/game" "$ROOT/usr/lib/ralert/game"
sha1sum "$ROOT/usr/lib/ralert/game"
