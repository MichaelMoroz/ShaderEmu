/*
 * A first /init for the initramfs, until a libc and busybox exist: a line to the console, then every line read from
 * it written back, forever. Code and its string only, PC-relative (lla): a flat program with no relocations
 * (mkflat.py). The kernel opened /dev/console as fds 0, 1 and 2.
 */
	.text
	.globl _start
_start:
	li a0, 1
	lla a1, banner
	li a2, banner_end - banner
	li a7, 64			/* write */
	syscall
	addi sp, sp, -128
1:	li a0, 0
	mv a1, sp
	li a2, 128
	li a7, 63			/* read */
	syscall
	blez a0, 1b
	mv a2, a0
	li a0, 1
	mv a1, sp
	li a7, 64
	syscall
	j 1b

banner:
	.ascii "gpu_vm init: user mode on gpu_vm's Linux. Type a line: it comes back.\n"
banner_end:
