/* tp is r4. TLS variant 1 as lld lays it out for gpu_vm (RISC-V's): the program's block starts at tp, struct pthread
   just below it. */
static inline uintptr_t __get_tp()
{
	uintptr_t tp;
	__asm__ ("mv %0, tp" : "=r"(tp));
	return tp;
}

#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 0

#define DTP_OFFSET 0

#define MC_PC __gregs[0]
