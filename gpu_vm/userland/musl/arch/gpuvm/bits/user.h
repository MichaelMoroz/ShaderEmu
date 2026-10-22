/* The kernel's user_regs_struct (arch/gpuvm/include/uapi/asm/ptrace.h): word n is rn, word 0 the pc. Floats are in
   the same registers: no float regset. */
#define ELF_NGREG 64
typedef unsigned long elf_greg_t, elf_gregset_t[ELF_NGREG];
typedef struct { unsigned __unused; } elf_fpregset_t;
