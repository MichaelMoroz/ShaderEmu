/* A compiler bug found building the machine's programs (gpu_vm/userland/apps/README.md, "Compiler bugs"): the fill
 * across blocks (GPUVMGlobalFill.cpp) makes a load before the branch that guards it, from a pointer that is 0 when
 * the branch is not taken: QuickJS (2026-06-04) faults at startup in JS_GetOwnPropertyInternal (`lw t2, -4(t1)` with
 * t1 = 0, in the bundle of `bltui t0, -9`: a JSValue's reference count read before its tag is tested). Without the
 * fill (-mllvm -gpuvm-global-fill=0), with -mllvm -gpuvm-spec-window=0 or -mllvm -gpuvm-fill-pairs=0 it runs.
 *   gpuvm-linux-cc -O2 spec_load.c -o spec_load    then on gvm_cpu: spec_load    -> SIGSEGV (load fault at 0xfffffffc)
 *   llvm-objdump -d -z: copy_out is
 *       lw t0, 4(t1); lw t1, 0(t1) ;;
 *       lw t2, -4(t1); addi t2, t2, 1; bltui t0, -9, ... ;;      <- the header read before the tag's test
 *   The cause, as it looks: `lw t1, 0(t1)` is taken for an access through t1 near -4(t1), but it writes t1: the
 *   access is through the old value. (dup_value, the same test on a value passed in registers, is compiled right.)
 */
#include <stdio.h>

typedef struct { int ref_count; } header;
typedef struct { union { void *ptr; int i; } u; int tag; } value;

static int seen;

__attribute__((noinline)) static value dup_value(value v)
{
	/* (QuickJS's JS_DupValue: only tags from -9 up, as unsigned, have a header) */
	if ((unsigned)v.tag >= (unsigned)-9) {
		header *h = (header *)((char *)v.u.ptr - 4);
		h->ref_count++;
		seen += h->ref_count;
	}
	return v;
}

/* (the value loaded from a pointer into that pointer's register, then its header read: QuickJS's js_dup of a
 * property's value. The fill takes `lw p, 0(p)` as an access through p that makes `lw x, -4(p)` safe: it is not, p
 * is another value after it) */
__attribute__((noinline)) int copy_out(value **pp, value *out)
{
	value v = **pp;

	if ((unsigned)v.tag >= (unsigned)-9)
		((header *)((char *)v.u.ptr - 4))->ref_count++;
	*out = v;
	return 1;
}

__attribute__((noinline)) int sum(value *vs, int n)
{
	int s = 0, k;
	value out, *p;
	for (k = 0; k < n; k++) s += dup_value(vs[k]).tag;
	for (k = 0; k < n; k++) p = &vs[k], s += copy_out(&p, &out) + out.tag;
	return s;
}

int main(void)
{
	static int cell[2] = { 5, 0 };
	value vs[3] = { { { .i = 0 }, 0 }, { { .ptr = &cell[1] }, -1 }, { { .i = 7 }, 1 } };

	printf("spec_load: %d %d (want 3 6)\n", sum(vs, 3), seen);
	return 0;
}
