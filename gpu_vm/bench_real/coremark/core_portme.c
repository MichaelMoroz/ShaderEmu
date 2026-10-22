/* CoreMark's port for a program under Linux (core_portme.h): CLOCK_MONOTONIC in microseconds. */
#include <time.h>
#include "coremark.h"
#include "core_portme.h"

static CORETIMETYPE start_time_val, stop_time_val;

static CORETIMETYPE now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (CORETIMETYPE)((ee_u32)t.tv_sec * 1000000u + (ee_u32)(t.tv_nsec / 1000));
}

void start_time(void) { start_time_val = now_us(); }
void stop_time(void) { stop_time_val = now_us(); }
CORE_TICKS get_time(void) { return stop_time_val - start_time_val; }
secs_ret time_in_secs(CORE_TICKS ticks) { return (secs_ret)ticks / 1000000.0; }

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[]) {
  p->portable_id = 1;
}
void portable_fini(core_portable *p) { p->portable_id = 0; }
