# vfork: clone(CLONE_VM | CLONE_VFORK | SIGCHLD, 0); no MMU, so no fork. The child runs on the parent's stack until
# it calls execve or _exit; this function keeps nothing on the stack (ra stays in its register).
	.text
	.global vfork
	.type vfork, %function
	.hidden __syscall_ret
	.p2align 4
vfork:
	li a0, 0x4111
	li a1, 0
	li a7, 220		# clone
	syscall
	j __syscall_ret
