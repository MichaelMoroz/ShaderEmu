#!/usr/bin/env bash
# The browser's start page for gpu_vm (run by gpu_vm/userland/apps/build.sh on its copy, $R): it names the machine.
set -euo pipefail
f="$R/linux/apps/web/about.html"
sed -i 's/a 32-bit RISC-V computer/a 32-bit GPU-VM computer/' "$f"
grep -q 'a 32-bit GPU-VM computer' "$f" || { echo "fix/web.sh: about.html's first line moved"; exit 1; }
