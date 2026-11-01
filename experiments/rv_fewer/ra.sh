#!/usr/bin/env bash
# Red Alert built in the private home on this tree's Tiberian Dawn patch (it applies that patch and takes cxxrt.cpp), made flat
# into the private root: bash experiments/rv_fewer/ra.sh
export REALHOME=$HOME
export HOME=$REALHOME/agents/fewer
export PYTHONUSERBASE=$REALHOME/.local
export NM=$HOME/nm W=$HOME/nm/work
export PATH=$W/bin:$REALHOME/shaderemu-linux/toolchain/bin:$PATH
bash linux/nommu/build.sh ralert/build.sh
tail -3 "$NM/logs/ralert-build.sh.log"
grep -nE 'error|Error |undefined|patch failed|does not apply' "$NM/logs/ralert-build.sh.log" | head -20
ls -la "$W/src/ralert-build/" 2>/dev/null | head -5
