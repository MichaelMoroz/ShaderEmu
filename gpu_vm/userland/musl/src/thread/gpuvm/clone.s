# __clone(func, stack, flags, arg, ...): ptid, tls, ctid are variadic, so on the stack (gpu_vm's convention: every
#           a0     a1     a2    a3     variadic argument in a 4-byte slot from sp)
# syscall(SYS_clone, flags, stack, ptid, tls, ctid): the kernel's CLONE_BACKWARDS order
#            a7       a0     a1     a2    a3   a4
# func and arg go on the child's stack; the child calls func(arg) and exits with its result.
	.text
	.global __clone
	.hidden __clone
	.type __clone, %function
	.p2align 4
__clone:
	andi a1, a1, -16
	addi a1, a1, -16
	sw a0, 0(a1)
	sw a3, 4(a1)
	mv a0, a2
	lw a2, 0(sp)
	lw a3, 4(sp)
	lw a4, 8(sp)
	li a7, 220		# clone
	syscall
	beqz a0, 1f
	ret
1:	lw t0, 0(sp)
	lw a0, 4(sp)
	callr t0
	li a7, 93		# exit
	syscall
