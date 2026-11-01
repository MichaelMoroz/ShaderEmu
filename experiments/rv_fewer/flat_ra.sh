#!/usr/bin/env bash
# The private build's Red Alert, flat, into a root: flat_ra.sh ROOT
set -euo pipefail
ROOT=$1
HOME_=$HOME/agents/fewer
export PATH=$HOME/shaderemu-linux/toolchain/bin:$PATH
T=$HOME_/flat_tmp; rm -rf "$T"; mkdir -p "$T/usr/lib/ralert"
cp "$HOME_/nm/repo/build/images/linux/root/usr/lib/ralert/game" "$T/usr/lib/ralert/game"
riscv32-linux-strip "$T/usr/lib/ralert/game" || true
python3 linux/nommu/flat.py "$T" > /dev/null
cp "$T/usr/lib/ralert/game" "$ROOT/usr/lib/ralert/game"
sha1sum "$ROOT/usr/lib/ralert/game"
