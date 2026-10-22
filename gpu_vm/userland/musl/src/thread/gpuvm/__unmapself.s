# __unmapself(base, size): munmap the thread's own stack, then exit, touching no stack between.
	.text
	.global __unmapself
	.type __unmapself, %function
	.p2align 4
__unmapself:
	li a7, 215		# munmap
	syscall
	li a0, 0
	li a7, 93		# exit
	syscall
