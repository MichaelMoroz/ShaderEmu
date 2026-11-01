#!/usr/bin/env bash
# The private build's game, flat, into a root (WSL path relative to the worktree); the ELF kept as logs for prof and addr2line
set -euo pipefail
ROOT=$1; NAME=$2
HOME_=/home/micha/agents/fewer; W=$HOME_/nm/work
export PATH=/home/micha/shaderemu-linux/toolchain/bin:$PATH
T=$HOME_/flat_tmp; rm -rf "$T"; mkdir -p "$T/usr/lib/tdawn"
riscv32-linux-strip -o "$T/usr/lib/tdawn/game" "$W/src/tdawn-build/vanillatd"
python3 linux/nommu/flat.py "$T" > /dev/null
cp "$T/usr/lib/tdawn/game" "$ROOT/usr/lib/tdawn/game"
mkdir -p $HOME_/elfs; cp "$W/src/tdawn-build/vanillatd" "$HOME_/elfs/$NAME"
sha1sum "$ROOT/usr/lib/tdawn/game"
