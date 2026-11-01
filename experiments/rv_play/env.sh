#!/usr/bin/env bash
# The private home's environment. env.sh build SCRIPT... [ENV=VALUE ...]   env.sh run COMMAND...   env.sh flat ROOT PROGRAM SRC  (a program made flat into a root, its ELF kept)
export REALHOME=$HOME
export HOME=$REALHOME/agents/play
export PYTHONUSERBASE=$REALHOME/.local
export NM=$HOME/nm W=$HOME/nm/work WORK=$HOME/nm/work
export PATH=$W/bin:$REALHOME/shaderemu-linux/toolchain/bin:$PATH
case "${1:-}" in
    build) shift; s=(); for e in "$@"; do case "$e" in *=*) export "$e";; *) s+=("$e");; esac; done
           bash linux/nommu/build.sh "${s[@]}" ;;
    run) shift; "$@" ;;
    flat) # ROOT(relative to the worktree) PATH-IN-ROOT ELF
        ROOT=$2; P=$3; E=$4; T=$HOME/flat_tmp; rm -rf "$T"; mkdir -p "$T/$(dirname "$P")"
        riscv32-linux-strip -o "$T/$P" "$E"
        python3 linux/nommu/flat.py "$T" > /dev/null
        cp "$T/$P" "$ROOT/$P"; sha1sum "$ROOT/$P" ;;
esac
