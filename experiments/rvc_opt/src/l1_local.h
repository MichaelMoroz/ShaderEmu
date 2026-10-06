// The write cache as a parameter of every function that reaches it, so that it can be a
// local array of the tick (L1_LOCAL), which unlike a static one is not zeroed every run.
// A function listed here is defined as NAME_l1(L1P ...); one without arguments is called as NAME_l1(L1A0).
#if defined(L1_LOCAL) && defined(PASS_TICK)
#define L1P inout uint4 l1_cache[L1_ENTRIES],
#define L1P0 inout uint4 l1_cache[L1_ENTRIES]
#define L1A l1_cache,
#define L1A0 l1_cache
#else
#define L1P
#define L1P0
#define L1A
#define L1A0
#endif
#define encode(a0) encode_l1(L1A a0)
#define fast_exec(a0) fast_exec_l1(L1A a0)
#define fast_run(a0) fast_run_l1(L1A a0)
#define ins_select(a0, a1) ins_select_l1(L1A a0, a1)
#define load_page(a0) load_page_l1(L1A a0)
#define mem_get_cached_or_tex(a0) mem_get_cached_or_tex_l1(L1A a0)
#define mem_get_word(a0) mem_get_word_l1(L1A a0)
#define mem_set(a0, a1, a2) mem_set_l1(L1A a0, a1, a2)
#define mem_set_byte(a0, a1, a2) mem_set_byte_l1(L1A a0, a1, a2)
#define mem_set_ram(a0, a1, a2) mem_set_ram_l1(L1A a0, a1, a2)
#define mmu_translate(a0, a1, a2) mmu_translate_l1(L1A a0, a1, a2)
