#!/bin/bash
# The worker firmware (gpu_vm/platform.md, "Workers"): worker.S linked at GVM_WORKER_FW (0x400) into worker_fw.h, the
# bytes and the symbols the hosts need (gvm_cpu, gvm_gpu, rvc_harness --gvm). Run in WSL; LLVM_BIN the gpuvm clang.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LLVM_BIN="${LLVM_BIN:-$HOME/gpu_vm/llvm-build/bin}"
T=$(mktemp -d)
"$LLVM_BIN/clang" --target=gpuvm-unknown-elf -c "$HERE/worker.S" -o "$T/worker.o"
"$LLVM_BIN/ld.lld" -m elf32gpuvm -e fw_entry --image-base=0 -Ttext=0x400 -o "$T/worker.elf" "$T/worker.o"
"$LLVM_BIN/llvm-objcopy" -O binary "$T/worker.elf" "$T/worker.bin"
"$LLVM_BIN/llvm-nm" "$T/worker.elf" > "$T/worker.nm"
mkdir -p "$HERE/../../build/gvm_platform"; "$LLVM_BIN/llvm-objdump" -d "$T/worker.elf" > "$HERE/../../build/gvm_platform/worker.dis" || true
python3 - "$T/worker.bin" "$T/worker.nm" "$HERE/worker_fw.h" <<'EOF'
import sys
data = open(sys.argv[1], 'rb').read()
syms = {}
for line in open(sys.argv[2]):
    p = line.split()
    if len(p) == 3 and p[2].startswith('fw_'): syms[p[2]] = int(p[0], 16)
assert syms['fw_entry'] == 0x400, syms
assert 0x400 + len(data) <= 0x1000, 'the firmware must end below the device tree (0x1000)'
data += b'\0' * (-len(data) % 16)
out = ['// gpu_vm/platform/worker.S, assembled by gpu_vm/platform/build.sh: do not edit (gpu_vm/platform.md, "Workers")',
       '#pragma once', '#include <stdint.h>',
       '#define GVM_WORKER_FW 0x400u        // where the host puts it: the boot area',
       '#define GVM_WORKER_SAVE 0x00c0c000u // its save area, 64 bytes a core',]
for k in ('fw_entry', 'fw_park', 'fw_park_end', 'fw_trap'):
    out.append('#define GVM_%s 0x%xu' % (k.upper(), syms[k]))
out.append('static const uint32_t gvm_worker_fw[%d] = {' % (len(data) // 4))
words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
for i in range(0, len(words), 8):
    out.append('    ' + ', '.join('0x%08x' % w for w in words[i:i + 8]) + ',')
out.append('};')
open(sys.argv[3], 'w').write('\n'.join(out) + '\n')
print('worker_fw.h: %d bytes, %s' % (len(data), ', '.join('%s %#x' % kv for kv in sorted(syms.items(), key=lambda kv: kv[1]))))
EOF
rm -rf "$T"
