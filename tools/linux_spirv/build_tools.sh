#!/usr/bin/env bash
export PATH=$HOME/vrc/ninja:$PATH
cd ~/vrc/dxbc-spirv
cat meson_options.txt
ls tools tools/* | head -40
python3 ~/vrc/meson/meson.py configure build -Denable_tools=true 2>&1 | tail -2
python3 ~/vrc/meson/meson.py configure build 2>/dev/null | grep -i -E 'tool|test' | head
~/vrc/ninja/ninja -C build 2>&1 | tail -3
find build -maxdepth 3 -type f -executable | grep -v -E '\.so|sanity' | head -20
