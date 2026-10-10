#!/usr/bin/env bash
# SPIRV-Tools from Ubuntu's own package, unpacked under ~/vrc (no root), and the validator on the files given.
mkdir -p ~/vrc/pkg && cd ~/vrc/pkg
if [ ! -x usr/bin/spirv-val ]; then
    apt-get download spirv-tools > /dev/null 2>&1
    for d in *.deb; do dpkg -x "$d" .; done
fi
export PATH=$HOME/vrc/pkg/usr/bin:$PATH
spirv-val --version 2>&1 | head -2
cd "${OUT:-.}"
for f in "$@"; do
    echo "== $f"
    spirv-val --target-env vulkan1.3 "$f" 2>&1 | head -12
    echo "exit ${PIPESTATUS[0]}"
done
