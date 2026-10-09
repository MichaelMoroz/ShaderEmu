// The write cache as a parameter of every function that reaches it, so that it can be a
// local array of the tick (L1_LOCAL), which unlike a static one is not zeroed every run.
// A function listed here is defined as NAME_l1(L1P ...); one without arguments is called as NAME_l1(L1A0).
//
// The second-level TLB's two arrays travel with it for the same reason (mmu.h): they have an
// occupancy bitmap as well, so neither needs to start out zeroed.
#ifndef TLB2_N
#define TLB2_N 256
#endif
//
// This needs a compiler that works on an inout array in place. fxc2 (which defines __FXC2__) and
// DXC do; FXC copies the array at every call and gives up, so it keeps the static arrays unless
// L1_LOCAL is asked for by name. L1_STATIC keeps them with fxc2 as well.
// L1_SHARED (the compute experiment): the arrays are thread group shared memory. That is not
// zeroed either, which the occupancy bitmaps already allow for. XR_SHARED: the registers too.
#if defined(L1_SHARED) && defined(SHADER_STAGE_COMPUTE)
#define L1_STATIC
#define L1_STORAGE groupshared
#else
#define L1_STORAGE static
#endif
#if defined(XR_SHARED) && defined(SHADER_STAGE_COMPUTE)
#define XR_STORAGE groupshared
#else
#define XR_STORAGE static
#endif
#if defined(__FXC2__) && !defined(L1_STATIC) && !defined(L1_LOCAL)
#define L1_LOCAL
#endif
#if defined(L1_LOCAL) && defined(PASS_TICK)
#if L1_WAYS == 4
#define L1_CACHE_P inout uint4 l1_cache[L1_DATA_N], inout uint4 l1_tag[L1_BUCKETS]
#define L1_CACHE_A l1_cache, l1_tag
#else
#define L1_CACHE_P inout uint4 l1_cache[L1_DATA_N]
#define L1_CACHE_A l1_cache
#endif
#if defined(NO_PAGING) || defined(M_MODE_ONLY)
#define L1_ARRAYS_P L1_CACHE_P
#define L1_ARRAYS_A L1_CACHE_A
#else
#define L1_ARRAYS_P L1_CACHE_P, inout uint tlb2_tag[3 * TLB2_N], inout uint tlb2_pg[3 * TLB2_N]
#define L1_ARRAYS_A L1_CACHE_A, tlb2_tag, tlb2_pg
#endif
#define L1P L1_ARRAYS_P,
#define L1P0 L1_ARRAYS_P
#define L1A L1_ARRAYS_A,
#define L1A0 L1_ARRAYS_A
#else
#define L1P
#define L1P0
#define L1A
#define L1A0
#endif
#define tlb_state_texel(a0, a1) tlb_state_texel_l1(L1A a0, a1)
#define encode(a0) encode_l1(L1A a0)
#define fast_exec(a0) fast_exec_l1(L1A a0)
#define fast_run(a0) fast_run_l1(L1A a0)
#define ins_select(a0, a1) ins_select_l1(L1A a0, a1)
#define load_page(a0) load_page_l1(L1A a0)
#define l1_find(a0) l1_find_l1(L1A a0)
#define mem_get_cached_or_tex(a0) mem_get_cached_or_tex_l1(L1A a0)
#define mem_get_word(a0) mem_get_word_l1(L1A a0)
#define mem_set(a0, a1, a2) mem_set_l1(L1A a0, a1, a2)
#define mem_set_byte(a0, a1, a2) mem_set_byte_l1(L1A a0, a1, a2)
#define mem_set_ram(a0, a1, a2) mem_set_ram_l1(L1A a0, a1, a2)
#define mmu_translate(a0, a1, a2) mmu_translate_l1(L1A a0, a1, a2)
