#!/usr/bin/env bash
# The tree edited apart from builds: upstream's commit with linux/tdawn/vanilla-conquer.patch, at ~/agents/fewer/vc.
#   vc.sh init     make it from the patch     vc.sh save     write the tree's diff to the patch
set -euo pipefail
E=$HOME/agents/fewer/vc
P=linux/tdawn/vanilla-conquer.patch
case "$1" in
  init) rm -rf "$E"; git clone -q $HOME/agents/fewer/nm/work/src/vanilla-conquer "$E"; git -C "$E" checkout -q -f ce83b59; git -C "$E" clean -qfd
        git -C "$E" apply "$PWD/$P"; git -C "$E" diff --stat | tail -1 ;;
  save) git -C "$E" diff > "$P.new"; mv "$P.new" "$P"; wc -l "$P" ;;
esac
