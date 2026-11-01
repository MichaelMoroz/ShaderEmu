#!/usr/bin/env bash
# In the second home: build the tree given by folder $1 (the old one), then this one over it, as a checkout that is pulled does.
# The stripped game must be the one a clean build gives: home2_over.sh OLD_WORKTREE
set -euo pipefail
OLD=$1; NEW=$PWD; R=$HOME; B=$R/agents/fewer_second_home
export REALHOME=$R HOME=$B PYTHONUSERBASE=$R/.local NM=$B/nm W=$B/nm/work
export PATH=$W/bin:$R/shaderemu-linux/toolchain/bin:$PATH
(cd "$OLD" && bash linux/nommu/build.sh tdawn/build.sh)
bash linux/nommu/build.sh tdawn/build.sh
"$R/shaderemu-linux/toolchain/bin/riscv32-linux-strip" -o "$B/game" "$W/src/tdawn-build/vanillatd"
sha1sum "$B/game" "$R/agents/fewer/game.stripped"
