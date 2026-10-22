/* gpu_vm's amo ops (a word, atomic on the core; gpu_vm/GPU_VM.md). amocas: rd in the expected word, out the old. */
#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	__asm__ __volatile__ ("amocas %0, %2, (%1)" : "+r"(t) : "r"(p), "r"(s) : "memory");
	return t;
}

#define a_swap a_swap
static inline int a_swap(volatile int *p, int v)
{
	int old;
	__asm__ __volatile__ ("amoswap %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
	return old;
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
	int old;
	__asm__ __volatile__ ("amoadd %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
	return old;
}

#define a_and a_and
static inline void a_and(volatile int *p, int v)
{
	int old;
	__asm__ __volatile__ ("amoand %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
}

#define a_or a_or
static inline void a_or(volatile int *p, int v)
{
	int old;
	__asm__ __volatile__ ("amoor %0, %2, (%1)" : "=r"(old) : "r"(p), "r"(v) : "memory");
}

#define a_barrier a_barrier
static inline void a_barrier()
{
	__asm__ __volatile__ ("fence" : : : "memory");
}

#define a_ctz_32 a_ctz_32
static inline int a_ctz_32(unsigned long x)
{
	return __builtin_ctz(x);
}

#define a_clz_32 a_clz_32
static inline int a_clz_32(unsigned long x)
{
	return __builtin_clz(x);
}
