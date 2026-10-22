/* A bFLT program's start (gpu_vm/userland/README.md): binfmt_flat leaves argc, argv and envp on the stack but no
   auxiliary vector, which musl reads after envp. __gvm_flat_start (src/env/gpuvm) makes the vector again with one
   (the page size, the ids, and a program header for the TLS, which elf2flt fills in) and calls START_c with it. */
__asm__(
".text\n"
".global " START "\n"
".type " START ",%function\n"
".p2align 4\n"
START ":\n"
"	mv a0, sp\n"
"	andi sp, sp, -16\n"
"	la a1, " START "_c\n"
"	call __gvm_flat_start\n"
"	unimp\n"
);
