#!/usr/bin/env bash
# Fetches the shader translator the Proton layers share (DXVK and vkd3d-proton: dxbc-spirv)
# and the build tools it wants, from their sources, into ~/vrc (nothing is installed system-wide).
set -e
mkdir -p ~/vrc && cd ~/vrc
[ -d dxbc-spirv ] || git clone -q --recursive https://github.com/doitsujin/dxbc-spirv
[ -d meson ] || git clone -q --depth 1 -b 1.6.1 https://github.com/mesonbuild/meson
if [ ! -x ninja/ninja ]; then
    [ -d ninja ] || git clone -q --depth 1 -b v1.12.1 https://github.com/ninja-build/ninja
    (cd ninja && python3 configure.py --bootstrap > /dev/null 2>&1)
fi
export PATH=$HOME/vrc/ninja:$PATH
cd dxbc-spirv
git log --oneline -1
[ -d build ] || python3 ~/vrc/meson/meson.py setup build --buildtype=release 2>&1 | tail -4
~/vrc/ninja/ninja -C build 2>&1 | tail -3
find build -maxdepth 3 -type f -executable | grep -v '\.so' | head -20
