#!/usr/bin/env bash
# Names of addresses as the machine has them (flat game at 0x85000040): a2l.sh ELF ADDR...   ELF "kernel": the kernel's vmlinux
E=$1; shift
export PATH=$HOME/shaderemu-linux/toolchain/bin:$PATH
for a in "$@"; do
  o=$(printf '%x' $((0x$a - 0x85000040)))
  echo "== $a"; riscv32-linux-addr2line -f -i -C -e "$E" "$o" | sed 's#/.*/##'
done
