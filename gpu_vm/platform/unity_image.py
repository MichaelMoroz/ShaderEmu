#!/usr/bin/env python3
"""gpu_vm's boot image for the Unity world (unity/gpuvm): RAM from address 0 as the harness makes it at start
(harness/apps/gvm_backend11.cpp): the boot area's stub at 0, the worker firmware at GVM_WORKER_FW, the device tree at
GVM_DTB and vmlinux's segments, up to the kernel's end. Writes DIR/unity_ram.bin and DIR/unity_boot.txt (the entry,
RAM's megabytes); the world's editor script (ShaderEmuGvm.cs) turns them and rootfs.romfs into its textures.

    python gpu_vm/platform/unity_image.py build/gvm_apps/img-cur
"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def header_consts(path):
    text = open(path).read()
    return {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define (\w+) (0x[0-9a-fA-F]+)u?\b", text)}


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "build/gvm_apps/img-cur"
    isa = header_consts(os.path.join(HERE, "..", "isa.h"))
    fwh = open(os.path.join(HERE, "worker_fw.h")).read()
    fw_at = header_consts(os.path.join(HERE, "worker_fw.h"))["GVM_WORKER_FW"]
    fw = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]{8}", fwh.split("{", 1)[1])]
    elf = open(os.path.join(d, "vmlinux"), "rb").read()
    assert elf[:4] == b"\x7fELF", "vmlinux is not an ELF"
    entry, phoff = struct.unpack_from("<II", elf, 24)
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    segs, end = [], 0
    for i in range(phnum):
        kind, off, vaddr, _, filesz, memsz = struct.unpack_from("<6I", elf, phoff + i * phentsize)
        if kind == 1:
            segs.append((vaddr, elf[off:off + filesz]))
            end = max(end, vaddr + memsz)
    ram = bytearray((end + 15) & ~15)
    # the boot area: { sync ; j 0 } at 0, where a trap with no handler lands, then the worker firmware
    ctrl_j = 4 << 7   # GVM_CTRL_J: GVM_CC_BEQ << 7
    struct.pack_into("<4I", ram, 0, isa["GVM_X_SYNC"] << 12, 0, 0, 1 << 3 | ctrl_j)
    struct.pack_into("<%dI" % len(fw), ram, fw_at, *fw)
    dtb = open(os.path.join(d, "gvm.dtb"), "rb").read()
    assert isa["GVM_DTB"] + len(dtb) <= isa["GVM_DEV"], "gvm.dtb does not fit below the devices"
    ram[isa["GVM_DTB"]:isa["GVM_DTB"] + len(dtb)] = dtb
    for vaddr, data in segs:
        ram[vaddr:vaddr + len(data)] = data
    ram_mb = int(open(os.path.join(d, "ram_mb")).read()) if os.path.exists(os.path.join(d, "ram_mb")) else 192
    open(os.path.join(d, "unity_ram.bin"), "wb").write(ram)
    open(os.path.join(d, "unity_boot.txt"), "w").write("entry %08x\nram_mb %d\n" % (entry, ram_mb))
    print("unity_ram.bin: %d bytes (to %08x), entry %08x, %d MB of RAM" % (len(ram), end, entry, ram_mb))


if __name__ == "__main__":
    main()
