/* A compiler bug found building the machine's programs (gpu_vm/userland/apps/README.md, "Compiler bugs"):
 * a call to a function of the same section more than 2 MB away is resolved by the assembler, which then fails
 * ("error: branch target out of range"), instead of being left to lld as R_GVM_J18 (which adds a range thunk).
 * QuickJS's quickjs.c (2026-06-04) has 2.3 MB of text in one .text and fails so 16 times.
 *   clang --target=gpuvm-unknown-linux -O2 -c far_call.c           -> error: branch target out of range
 *   clang --target=gpuvm-unknown-linux -O2 -ffunction-sections -c far_call.c   -> fine (lld makes the thunk)
 */
static __attribute__((noinline)) int near_fn(int x)
{
	return x * 3 + 1;
}

/* 2.25 MB of code between the call and the function it calls */
static __attribute__((noinline)) void padding(void)
{
	__asm__ volatile(".rept 147456\n nop ;;\n .endr");
}

int main(int argc, char **argv)
{
	(void)argv;
	padding();
	return near_fn(argc);
}
