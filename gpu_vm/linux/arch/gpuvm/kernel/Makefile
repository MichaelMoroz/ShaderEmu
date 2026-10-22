# SPDX-License-Identifier: GPL-2.0

extra-y := head.o vmlinux.lds

obj-y := entry.o traps.o irq.o time.o setup.o process.o signal.o ptrace.o \
	 syscall_table.o sys_gpuvm.o reset.o

obj-$(CONFIG_GPUVM_CONSOLE) += console.o
obj-$(CONFIG_GPUVM_ROM) += romblk.o

ccflags-$(CONFIG_GPUVM_CONSOLE) += -I$(srctree)/drivers/tty/hvc
