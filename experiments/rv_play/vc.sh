#!/usr/bin/env bash
# The game's tree edited apart from builds, at ~/agents/play/vc: vc.sh init | save | run SCRIPT (a python script run with the tree as its folder)
set -euo pipefail
E=$HOME/agents/play/vc
P=$PWD/linux/tdawn/vanilla-conquer.patch
case "$1" in
  init) rm -rf "$E"; git clone -q $HOME/agents/play/nm/work/src/vanilla-conquer "$E"; git -C "$E" checkout -q -f ce83b59; git -C "$E" clean -qfd
        git -C "$E" apply "$P"; git -C "$E" diff --stat | tail -1 ;;
  save) git -C "$E" diff > "$P.new"; mv "$P.new" "$P"; wc -l "$P" ;;
  run) cd "$E"; python3 "$2" ;;
  show) cd "$E"; shift; "$@" ;;
esac
