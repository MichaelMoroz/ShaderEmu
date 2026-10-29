#!/bin/bash
# the image NAME: the prebuilt no-MMU root with the openttd.bin built in ~/ofps (made flat)
NAME=$1; cd /e/Development/Emu/ShaderEmu-ofps
[ -d build/flat_of ] || cp -a linux/prebuilt/nommu/root build/flat_of
MSYS_NO_PATHCONV=1 wsl -- bash -lc "python3 /mnt/e/Development/Emu/ShaderEmu-ofps/linux/nommu/elf2bflt.py ~/ofps/repo/build/images/linux/root/usr/bin/openttd.bin /mnt/e/Development/Emu/ShaderEmu-ofps/build/flat_of/usr/bin/openttd.bin 0x100000" | grep -v "^$" | tail -2
python linux/nommu/image.py $NAME build/flat_of 2>&1 | tail -1
