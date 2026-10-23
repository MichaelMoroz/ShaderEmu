# gpu_vm: the ideas for 2x the RISC-V machine, and where each stands

Goal (23 October 2026): every idea below investigated; then a tick at least 2x the RISC-V machine (rvc_opt) on
realistic work (the game bench `gpu_vm/tick/gamebench.h`, calibrated against the games, confirmed by the games'
frames a second, flat out, on the card); then the compiler bringing compiled code to that number.

Method: the tick builder (`gpu_vm/tick/builder`) generates variants, each WARP-checked (correctness only, never
timed) and register-checked, then timed on the card in batches under 10 minutes; batch, analyze, report, repeat.

| # | Idea | Status | Result |
|---|---|---|---|
| 1 | Write cache read tag first, bloom word (x23) | done | Half-Life 108 -> 154 frames in 15 s; loads/stores loops -14% |
| 2 | x24: x23's memory ops (last stored texels in registers, first-bucket filter, texel fetched first) | done | loads +28%, stores +24% vs x23, gzip -6%, games as x23; as builder grafts on x23 all lose (g2a) |
| 3 | Core 0's own large write cache (x25, 1,026 entries) | done | core 0 2.4x bundles a pass, every game slower flat out (a pass 1.8-2.7x dearer) |
| 4 | Memory model study: stack share, coalescing, memcpy as the commit's copy, move on insertion | done (model) | stack 16-20% of distinct stored texels, globals 50-57%, memcpy 42%; move on insertion +17-23% in the model, within noise on the card |
| 5 | Active workers serialized by placement | done (g "dims") | not so: busy workers overlap fully; bit-reversed placement -10% |
| 6 | Idle-but-awake workers drawn every pass | measured | ~1.3 us each, 40 of them +2-3% |
| 7 | Large register file in the core's state (merged registers and cache), 64-1024 | measured (g1, g3) | an unused file costs 3-4% (128) to 12-16% (1024); with it the stack-in-file kernels gain 6-7% on the same tick (util 0.40 -> 0.48), net vs x23 +4% g_logic, +2% g_hl, -5% g_q3; lazy loading of the file -15-20% |
| 8 | Registers bound to addresses (fbind), (address, value) pair store buffer (fpair) | implemented (58970b6) | to measure |
| 9 | Budget toggle with a flush window | superseded | the tick spills registers at every pass end; full budget every pass is the target |
| 10 | Full budget every pass (core-0 util as a score) | in scoring | x23 util 0.45 on the game set |
| 11 | One local array vs several; packing scalars as uint4 (every scalar array is declared 4 wide: 21.7 KB a lane) | done (g4) | merged = split (0.981 vs 0.984); uint4 cache 0.87-0.88, uint4 registers 0.71, both 0.58: 5.5 KB a lane and slower everywhere (the ", 4" is no real cost; a dynamic component select is). Keep scalar split arrays |
| 12 | Wider bundles vs a thin loop; superinstructions from the profiles | in progress (g6, g7) | one op a bundle on today's decoder: a bundle 29% cheaper, work 18-33% slower (the kernels fill 1.3-1.4 ops a bundle); thin8 (8-byte instructions, reference + assembler + tick) passes the gate, on the card in g7; 3-4 ops / 128-bit registers next |
| 13 | Loads that skip the store check (read-only data, like `__ldg`) | done (g5) | GVM_W_NC (load width 5, lw_nc): p_rdnc = p_rd to 0.1%; the lookup behind the bloom word costs nothing measurable; not worth the width |
| 14 | A read cache for reused loads vs the GPU's texture cache | done (g5) | last 1 / 2 fetched texels in registers: -6% / -10% (loads -12..-25%); fetching ROM or RAM behind a branch instead of both: -6%. The texture cache wins |
| 15 | Large copies through the commit's operation queue | measured (bench `_c`) | as used for 128-byte struct copies, much slower: g_logic_c 24.4 frames/s against 61.9 (x23; x24 alike), g_hl_c 23.8 against 42.5; a store to GVM_OPS ends core 0's pass and a pass with a copy costs about 1.8 ms more on the card; worth it only for copies of many KB (keep memcpy's loads and stores for structs). Stack in registers (`_r`): g_logic 74.1 against 61.9 (+20%), g_hl 46.8 against 42.5 (+10%) |
| 16 | Compiler gap: hand assembly vs our clang vs gcc for RISC-V, same kernels | done (bench) | none: clang -O2 (the games' flags) makes 0-10% fewer bundles than hand assembly at the same ns a bundle (g_logic 63.8 vs 62.0 frames/s on x23, g_verts 34.5 vs 35.5, g_ray1 11.6 vs 11.5); gcc's RISC-V 1.75-2.4 instructions a bundle (games 1.7-3.0); x23 over RISC-V 1.46-1.88 one core, g_hl 1.18 (HL 1.10), g_ray 1.04 (nxray 0.86), g_q3 1.74 (Q3 0.79: bound by passes, not code); the earlier 2.2x was a RISC-V run beside WARP checks |
| 17 | Compiler bundling options (-gpuvm-sched-kind, -gpuvm-mask-select, -gpuvm-fnrel) | parked | unfinished patch in build/wip |
| 18 | More registers for LLVM; spills and frames in the core's file | after 7 and 16 | |
| 19 | 128-bit registers: SIMD (one opcode, four lanes) and lane-VLIW (an opcode a lane), predication by lane mask, 16-byte texel loads/stores | queued (builder; vector kernels from the bench) | |
| 21 | Compiler density on real game code (Q3 workers 1.0-1.4 RISC-V instructions a bundle vs 2.3 on kernels): soft double from a dropped -fsingle-precision-constant (>=10.8% of Q3 worker bundles), one-op bundles 41.9%, globals through lpc 5.3%, calls 4.8%, memcpy 2.8% | in progress (bench) | toolchain fix: -cl-single-precision-constant |
| 20 | Pass latency: Quake III is ~6.3 passes a frame with core 0 under a third of its budget (sync, system); per-pass fixed cost, worker budget size, dependency rounds per frame | modelling (bench) | the kernels are 1.5-1.9x RISC-V but Q3 0.79x |
