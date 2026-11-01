#!/usr/bin/env bash
# A private build home's environment and a build of Tiberian Dawn in it (linux/nommu/build.sh with HOME moved).
#   bash experiments/rv_fewer/env.sh build [ENV=VALUE ...]     the game; the ELF is $W/src/tdawn-build/vanillatd
#   bash experiments/rv_fewer/env.sh run COMMAND...            a command with the toolchain on the path
export REALHOME=$HOME
export HOME=$REALHOME/agents/fewer
export PYTHONUSERBASE=$REALHOME/.local
export NM=$HOME/nm W=$HOME/nm/work
export PATH=$W/bin:$REALHOME/shaderemu-linux/toolchain/bin:$PATH
case "${1:-}" in
    build) shift; for e in "$@"; do export "$e"; done; bash linux/nommu/build.sh tdawn/build.sh; tail -2 "$NM/logs/tdawn-build.sh.log"; ls -la "$W/src/tdawn-build/vanillatd" ;;
    run) shift; "$@" ;;
esac
