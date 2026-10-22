# SPDX-License-Identifier: GPL-2.0
#
# gpu_vm: clang only (make ARCH=gpuvm LLVM=1). The target triple is CLANG_TARGET_FLAGS_gpuvm in
# scripts/Makefile.clang (gpu_vm/linux/patches); override it on make's command line.

KBUILD_DEFCONFIG := gpuvm_defconfig

# char is signed in the gpu_vm ABI; said here too in case the target's default changes.
KBUILD_CFLAGS += -fsigned-char
# Bundles are 16 bytes: functions and every code label start at a bundle.
KBUILD_CFLAGS += -falign-functions=16

head-y := arch/gpuvm/kernel/head.o

core-y += arch/gpuvm/kernel/ arch/gpuvm/mm/
libs-y += arch/gpuvm/lib/

boot := arch/gpuvm/boot

# The raw image: what gvm_cpu loads with --bin FILE --at 0x02000000.
KBUILD_IMAGE := $(boot)/Image

all: Image

Image: vmlinux
	$(Q)$(MAKE) $(build)=$(boot) $(boot)/$@

archclean:
	$(Q)$(MAKE) $(clean)=$(boot)

define archhelp
  echo  '* Image        - the raw kernel image (arch/gpuvm/boot/Image), loaded at 0x02000000'
  echo  '  dtbs         - arch/gpuvm/boot/dts/gvm.dtb, for gvm_cpu --dtb'
endef
