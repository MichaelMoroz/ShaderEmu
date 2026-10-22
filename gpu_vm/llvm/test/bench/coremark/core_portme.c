/* CoreMark's port to gpu_vm on gvm_cpu (gpu_vm/llvm/test/bench): the clock is CYCLE, the bundles run. */
#include "coremark.h"
#include "core_portme.h"

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

static CORETIMETYPE start_time_val, stop_time_val;

static CORETIMETYPE cycles(void) {
  CORETIMETYPE c;
  __asm__ volatile("mfsr %0, cycle" : "=r"(c));
  return c;
}

void start_time(void) { start_time_val = cycles(); }
void stop_time(void) { stop_time_val = cycles(); }
CORE_TICKS get_time(void) { return stop_time_val - start_time_val; }
/* ten microseconds a bundle: a short run passes CoreMark's rule of 10 seconds (the
   bench reads the bundles) */
secs_ret time_in_secs(CORE_TICKS ticks) { return (secs_ret)ticks / 100000.0; }

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[]) {
  p->portable_id = 1;
}
void portable_fini(core_portable *p) { p->portable_id = 0; }
