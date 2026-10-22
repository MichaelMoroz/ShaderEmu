/* CoreMark's port for a program under Linux, the same source for gpu_vm's Linux and the RISC-V machine's
   (gpu_vm/bench_real): the clock is CLOCK_MONOTONIC in microseconds, the data static, the seeds and the
   iterations from the command line (coremark 0x0 0x0 0x66 ITERATIONS). */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H
#include <stddef.h>
#include <stdio.h>

#define HAS_FLOAT 1
#define HAS_TIME_H 1
#define USE_CLOCK 0
#define HAS_STDIO 1
#define HAS_PRINTF 1
#define COMPILER_VERSION "clang " __clang_version__
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS FLAGS_STR
#endif
#define MEM_LOCATION "STATIC"
#define MEM_METHOD MEM_STATIC
#define SEED_METHOD SEED_ARG
#define MULTITHREAD 1
#define MAIN_HAS_NOARGC 0
#define MAIN_HAS_NORETURN 0

typedef signed short ee_s16;
typedef unsigned short ee_u16;
typedef signed int ee_s32;
typedef double ee_f32;
typedef unsigned char ee_u8;
typedef unsigned int ee_u32;
typedef ee_u32 ee_ptr_int;
typedef size_t ee_size_t;

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x)-1) & ~3))

#define CORETIMETYPE ee_u32
typedef ee_u32 CORE_TICKS;

typedef struct CORE_PORTABLE_S {
  ee_u8 portable_id;
} core_portable;

extern ee_u32 default_num_contexts;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#if (TOTAL_DATA_SIZE == 1200)
#define PROFILE_RUN 1
#elif (TOTAL_DATA_SIZE == 2000)
#define PERFORMANCE_RUN 1
#else
#define VALIDATION_RUN 1
#endif
#endif

#define ee_printf printf
#endif
