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
| 12 | Wider bundles vs a thin loop; superinstructions from the profiles | queued | |
| 13 | Loads that skip the store check (read-only data, like `__ldg`) | queued | |
| 14 | A read cache for reused loads vs the GPU's texture cache | queued | |
| 15 | Large copies through the commit's operation queue | in the bench (`_c` kernels) | |
| 16 | Compiler gap: hand assembly vs our clang vs gcc for RISC-V, same kernels | in progress | the bench favours gpu_vm 2.2x more than the games do |
| 17 | Compiler bundling options (-gpuvm-sched-kind, -gpuvm-mask-select, -gpuvm-fnrel) | parked | unfinished patch in build/wip |
| 18 | More registers for LLVM; spills and frames in the core's file | after 7 and 16 | |
