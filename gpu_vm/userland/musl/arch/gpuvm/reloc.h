/* Static programs only (bFLT, no dynamic linker): what dynlink.h needs to compile. Numbers are gpu_vm/isa.h's. */
#define LDSO_ARCH "gpuvm"

#define TPOFF_K 0

#define REL_SYMBOLIC    1	/* R_GVM_32 */
#define REL_RELATIVE    9	/* R_GVM_RELATIVE */
#define REL_TPOFF       8	/* R_GVM_TPREL32 */

#define CRTJMP(pc,sp) __asm__ __volatile__( \
	"mv sp, %1 ; jr %0" : : "r"(pc), "r"(sp) : "memory" )
