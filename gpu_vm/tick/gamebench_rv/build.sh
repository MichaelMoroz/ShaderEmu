#!/bin/bash
# The game bench for the RISC-V machine (GAMEBENCH.md): gamebench.c with the workers' library, a flat program, and a
# private no-MMU image build/images/gamebench_rv (linux/prebuilt/nommu's root + /usr/bin/gamebench). In WSL.
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd)
export PATH="$HOME/nm/work/bin:$HOME/shaderemu-linux/toolchain/bin:$PATH"
O=$R/build/gamebench; mkdir -p $O
rv32-cc -O2 -w -I"$R/programs/mc" "$R/gpu_vm/tick/gamebench_rv/gamebench.c" "$R/programs/mc/mcw.c" -o $O/gamebench.elf -lm
riscv32-linux-strip -o $O/gamebench $O/gamebench.elf
[ "$1" = --no-image ] && exit 0
rm -rf $O/root; cp -a "$R/linux/prebuilt/nommu/root" $O/root
python3 "$R/linux/nommu/elf2bflt.py" $O/gamebench $O/root/usr/bin/gamebench 0x40000
chmod 755 $O/root/usr/bin/gamebench
echo built $O/root/usr/bin/gamebench
