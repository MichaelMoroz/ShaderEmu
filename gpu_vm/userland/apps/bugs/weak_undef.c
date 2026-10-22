/* An undefined weak function's address under gpuvm-linux-cc's -fPIE (gpu_vm/userland/apps/README.md, "Compiler bugs"):
 * `lpc` makes it from the bundle's address, so a symbol that is 0 at the link becomes the program's load address at
 * run time, and `if (f) f()` calls into the program's first bytes (nano: SIGILL at the text's start).
 *   gpuvm-linux-cc -O2 weak_undef.c -o weak_undef      on gvm_cpu: "weak_undef: f is 0x..." then SIGILL
 */
#include <stdio.h>

extern void missing_function(void) __attribute__((weak));
extern int missing_data __attribute__((weak));

int main(void)
{
	printf("weak_undef: missing_function is %p, missing_data is %p (want both 0)\n", (void *)missing_function, (void *)&missing_data);
	if (missing_function)
		missing_function();
	return 0;
}
