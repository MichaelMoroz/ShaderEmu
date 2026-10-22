# The start of a bare-metal gpu_vm program on gvm_cpu (gpu_vm/cpu/README.md):
# sp, a0 = argc and a1 = argv are set by gvm_cpu; main's result is the exit
# code.
	.text
	.globl	_start
	.p2align	4
_start:
	call	main
	call	exit
1:	j	1b
