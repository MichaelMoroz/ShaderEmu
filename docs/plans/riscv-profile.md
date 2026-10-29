# Where the RISC-V machine's time goes: the 51 parts profiled (28 October 2026)

The profile behind `riscv-backport-plan.md`. The parts are the 51 of `tools/rv_compare/parts.py` that the before/after
comparison measures. They ran on the frozen before image (`ShaderEmu-rvbp/build/before/build/images/nommu`) under
`rvc_cpu` with `RVC_PROF` and `RVC_WCACHE=1` (and again with `RVC_CALLS` for the jobs). The code is named from ELFs
built again from the image's sources (7624f55, built privately) and from the kernel at b935841.

## How it was measured

- `python tools/rv_compare/prof_parts.py PART [--mode prof|calls]` boots the image, starts `nano-X -p`, and runs the
  part's command and its sends. It profiles a window of the part:
  - from the part's `pace` line, or the command;
  - to its `until` line, or a span given in `WINDOWS`.

  Every run takes under 50 s of the host. `rvc_cpu` runs plain (fast handlers, workers on threads) until
  `--count-from`'s line. The output goes to `build/prof_parts/PART.prof`.
- Guest seconds are the model's: 0.25 ms a pass plus core 0's instructions at 3.7 M a second.
- "Write-cache entries" are `pcs.txt`'s new `newtex` column: stores that took a texel new to the core's pass, with
  bytes changed. "Passes ended full" are stores that found `RVC_WCACHE`'s cache full (`full`).
- Each table's tool:
  - `prof_summary.py`: kinds of code, top functions, GOT loads executed, jobs;
  - `prof_forms.py`: copies by call size, the kernel's `__memcpy` paths, soft-float callers, large fills;
  - `prof_kernel.py`: kernel functions;
  - `prof_doc.py`: these tables.
- The ELFs are a build a little apart from the image's. `prof.py` places them in pieces (`place_pieces`). A pc is
  named only where the instruction that ran is the ELF's there: the opcode and registers must match, while immediates
  may differ. Code from an ELF that was not the image's shows as `?0xPAGE`, and its owner was found by runs of words
  (below). Such code covers about a third of nxvideo's core 0, ClassiCube's pages, and the `memory` part's program,
  which is compiled in the guest by tcc and has no ELF.
- The kinds of code:
  - "libc copy/fill/str": musl's `mem*` and `str*`;
  - "libc other": the rest of musl;
  - "libm": musl's math objects;
  - "soft float, 64-bit": compiler-rt's `__*df3`, `__*tf3`, conversions and 64-bit divides;
  - "window server": nano-X;
  - "window client": the nano-X client library.
- The 18 parts that start an app from the Start menu (`tetris` ... `example-cube`, `desktop`) run `nx` first. Their
  window holds the session's start (nano-X, the bar, `nxpersist load`, about 32 programs started by the scripts)
  before the app's 8 s, which is why their profiles look alike.

Unknown pages that were named by runs of words:

| part | page | share of core 0 (%) | what it is |
|---|---|---|---|
| video-mpg | 0x819d6000 | 33.6 | `__udivdi3`, 95% of the page: called from `media_video_time`, 43,604 calls |
| video-mpg | 0x819ce000 | 6.0 | `__clzsi2`, `__divdf3` |
| video-avi | 0x8167c000, 0x8167d000 | 19.4 | `mcw_done`, `run_share`, `team_job`: core 0 waiting for and taking part in its jobs |
| classicube | 0x816ed000, 0x816bf000, 0x816fc000 | 34.8 | `Matrix_Mul` (61% of its page), `SineFrom`, `serve` (core 0 serving its workers) |
| nxphys | 0x8148f000, 0x81490000 | 25.5 | `mcw_recalled`, `mcw_done`: core 0 waiting for its workers |
| nxray | 0x8148f000 | 12.6 | `memset` |
| quake3, quake3-server | 0x82471000 | 8.3-8.8 | `mcw_done` (core 0 waiting for its jobs), `look` |
| quake3-menu | 0x823ac000 | 12.4 | `SE_DllSyscall0` (the UI module's system calls) |
| nes | 0x81659000 | 9.0 | `rc_slow_read`, `rc_slow_write` |
| glxgears | 0x818d1000, 0x818d2000 | 33.2 | a word-copy loop (`lw`/`sw`, 301,792 trips, nearly every store silent) found in no ELF built |
| memory | 0x81477000-0x81483000 | 61 | `memb`, the part's program, compiled in the guest |

## The plan's items, ranked by the profile

Ranked by how much of core 0 they would take away, and in how many parts. Core 0 is the frame's chain in every
game.

| rank | plan item | evidence (shares of core 0 unless said) | parts |
|---|---|---|---|
| 1 | Section 3, a program's memory in blocks (fastrun) | The kernel's page allocator and freeing (`free_pcppages_bulk` 10%, `__free_one_page` 5.5%, `alloc_pages_run` 5.5%, `free_unref_page` and its commit 4%) are about 26% of every start part. Measured inclusively, `exit_mmap` is 17% and `do_munmap` 8% (program exits, page by page), and `do_mmap` 7%. Kernel overall: 70% of the desktop start. A large `malloc` at a program's start: `alloc_pages_run` is 14.1% of nxweb, 12.0% of nxphys, 6.7% of nxpaint; nxview's frees are 23.5%. Mean over all 51 parts: `free_pcppages_bulk` 3.3%, `alloc_pages_run` 2.8%, `__free_one_page` 1.8%, the top three of the kernel | 18 start parts, nxweb, nxphys, nxview, nxpaint, nxray |
| 2 | Section 3, relocations a word at a time (the plan ranks it last) | `load_flat_binary` 5.0% plus `calc_reloc` 1.7% of every start part; nxweb 2.9 + 1.0, qjs 2.8 + 0.9. Mean over the parts: 1.65 + 0.54, the 4th and 11th kernel functions | the starts |
| 3 | Section 2, items 1 and 2: musl's `memcpy` through the commit's copy | quake-timedemo: the program's copies are 44.6% of core 0's write-cache entries, 2,542 of its 4,194 full passes are in `memcpy`, and 442 calls of 2 KB or more (11,363 instructions a call) are 5.6% of its instructions. Half-Life: copies are 11.2% of instructions and 18.9% of entries, with 290 distinct texels a busy pass, the most of any game. OpenTTD: `memmove` 38,739 calls of 767 instructions, and 609 fills of 4,386. Tiberian Dawn: copies are 16.1% of entries. tcc (`cc`): 16.1%. `memory`: 199 passes ended full a guest second | quake-timedemo, halflife, openttd, tdawn, cc, memory |
| 4 | Section 2, item 4: the kernel's misaligned copy | The byte paths of `__memcpy`: quake-timedemo 6.7% (1.52 M byte stores), doom-attract 3.6%, doom 2.1% (`copy_page_to_iter` from `read()`: 2,224 calls of about 1,916 instructions), nxfiles 1.2%, nxide 1.1%, htop 1.1%, the starts 0.6-0.8%. The 64-byte word loop also ended 211 of Doom's 888 full passes (from `read()`'s copies of 1 KB or more that are not 16-aligned and so stay off the commit's copy) | doom, doom-attract, quake-timedemo, nxfiles, nxide, htop, the starts |
| 5 | Section 1, floats in floats (libm) | `logf` in soft doubles: Quake 7.4%, quake-server 8.1%, Quake III 6.0%, quake3-server 5.8%, quake3-menu 3.4%, quake-timedemo 1.4%. Pinball's `sinf`/`cosf` (`__sindf` 9.0%, `__cosdf` 8.6%) make Pinball's libm and soft floats 19.9%. Quake's `Sys_FloatTime` 1.4% (doubles). nxpath's `__sindf`/`__cosdf` 1.1% | quake (3), quake3 (3), pinball, nxpath |
| 6 | Section 1, nano-X's filled shapes as one command | Pinball: nano-X is 42.6% of core 0 (`gpu_drawhorzline` 11.1%, `GdFillPoly` 1.9%) | pinball, clock (`GdFillPoly` 1.8%) |
| 7 | Section 1, Doom's level ahead on workers, no 64-bit divide in `clip` | doom-attract (the demos' level loads): `split` 3.4% and `__divdi3` 3.2% in soft 64-bit, `wall_texture` fills 0.44%, the kernel 32% (loading) | doom-attract |
| 8 | Section 1, Quake III's scene job is the frame's chain | Core 0 waits in `mcw_done` (page 0x82471000, 8.3-8.8% of core 0) in both Quake III parts | quake3, quake3-server |
| 9 | Section 3, the kernel's config (no `VM_EVENT_COUNTERS`, `BASE_SMALL`) | Not seen by name. `free_pcp_prepare`, `__put_page` and the vm counters are inside the allocator's 26% above | the starts |
| 10 | Section 2b, PIE and GOT loads | GOT loads executed on core 0: micropython 1.39%, nano-X 1.23% in Pinball, the rest 0.0-0.2% (doom 0.12, halflife 0.14). Places in the code (`auipc`+`lw` of a `.got` word): Half-Life 12,439, Quake III 4,087, Quake 2,305, micropython 786, nano-X 701, tdawn 545, doom 279. The workers' GOT loads were not counted | python, pinball |

## New candidates the plan does not have

Each one comes with its evidence. The shares are of core 0 in the part's window.

1. **nxvideo's `mpeg_ready` divides in 64 bits for every unit at every poll** (`linux/libmedia/m_video_mpeg.c:636`).
   `media_video_time(v, ...)` is `(uint64_t)n * 1000000 / fps_milli`: 43,604 calls, 37.9% of core 0 inclusive, and
   `__udivdi3` alone is 33.6%. `media_video_time(v, m->asked + 1)` is the same for every unit in the loop. Taking it
   out of the loop, and working in 32 bits or by a reciprocal kept in the video, takes most of it away. `media_video_at`
   is a further 4.3% (5.4% in video-avi).
2. **The music player's Vorbis tables in doubles.** nxplay's `init_blocksize` calls `sin` 2,560 times: `__cos` 15.6%,
   `__sin` 6.9%, `sin` 4.1%, and the libm and soft floats are 32.9% of music's core 0. Fix it with float sines (the plan's
   libm item, if `sin` goes to `sinf`) or tables made once.
3. **Tiberian Dawn's menu polls at full speed.** In tdawn-menu, `host_event` is 34.0%,
   `WWKeyboardClassShaderEmu::Fill_Buffer_From_System` 21.6% and `host_ms` 15.9%, at 3.32 M instructions a guest
   second (the most a core can run) while nothing happens. Fix it with `mcw_rest()` (or a sleep) when no event came.
   The idle desktop parts (nxedit, nxfiles) run 0.25 M a second, so the rest of the system does rest.
4. **OpenTTD decodes and encodes sprites on core 0 during play.** In the `openttd` window, `memcpy_any` is 19.0%,
   `DecodeSingleSprite` 16.3% and `Blitter_8bppOptimized::Encode` 14.8% (with its 609 fills of about 7 KB, 1.8%): about
   half of core 0, and 77 passes ended full a guest second. Fix it with a larger sprite cache, sprites made ahead on
   workers, or the encode's fill only over the rows it writes.
5. **The kernel's trap entry.** `handle_exception` is 1.6% of core 0 on average and 4.8% of its write-cache entries on
   average (the registers saved): video-avi 9.2%, video-mpg 7.2%, doom-attract 7.1%, nxray 6.2%, nes-nova 6.1%,
   nxphys 4.2%. Fewer traps (no `REST` ioctl when alone, as the plan's "Also small" says) or saving fewer registers on
   the system calls that need fewer.
6. **The kernel's timekeeping at every tick.** `timekeeping_advance` and `timekeeping_update` are 1.35% and 0.61% of
   core 0 and 2.5% and 2.3% of its write-cache entries on average, the top kernel functions of an idle desktop (nxedit
   6.5%, nxfiles 7.6%). Fix it with fewer ticks when idle (a tickless idle) or less timekeeping state written a tick.
7. **Floats printed through long double.** gzip's input is made by `seq`, through musl's `fmt_fp` in
   long double (`__multf3` 7.8%, `fmt_fp` 13.0% inclusive). This is the test's own input, but any `printf("%f")` pays
   it. Fix it with a `fmt_fp` in doubles, or in 64-bit integers for the common precisions.
8. **64-bit divides in libmedia's picture fit and in qjs's bigints.** nxview's `fit_rows` (`linux/libmedia/m_core.c`)
   is 9.9% inclusive, with `__divdi3` 8.9% and `__udivmoddi4` 8.2% under it. qjs's `js_bigint_to_string1` is 10.6%,
   with `__udivdi3` 5.1% and `__umoddi3` 4.7%. Fix `fit_rows` with per-row or per-column steps made once. Fix qjs with
   divides by 10^9 in 32 bits.
9. **The window server's 300 KB clear at its start.** `gpu_open`'s memset is 2 calls of 300,690 instructions, 2.2% of
   the desktop start. Fix it with the kernel's zero pool, or a fill by the commit.
10. **Micropolis scans every tile to animate.** `Micropolis::animateTiles` is 39.4% of core 0. Fix it with a list of
    the animated tiles, or the scan on workers.
11. **Quake's lightmap allocation and alias models at load.** In quake-timedemo, `AllocBlock` is 7.5%, and
    `GL_MakeAliasModelDisplayLists` ended 503 passes full. Both are first-frame costs a player sees as the level's
    load.
12. **htop's `memcmp`** is 14.6% of core 0 and its `memset` 4.7%: the screen compared whole each refresh.
13. **tcc's symbol lookups by `strcmp`** are 8.5% of `cc`'s core 0. tcc's `next_nomacro` is 10.4%.
14. **glxgears' word-copy loop.** It is 22% of core 0, 301,792 trips, and nearly every store is silent: a copy of a
    buffer onto itself, unchanged, each frame. It is in no ELF built from 7624f55 (the image's glxgears or gles is
    older), so its source still has to be found.
15. **Console prints in the frame.** In quake3-server, `n_tty_write` is 3.2% and `se_con_put` 1.6%; in Quake they are
    1.4% and 0.7%. Partly the parts' own stats prints, but every `Com_Printf` pays the tty's path.

## Plan items the profile says are worth little

- **PIE/GOT (section 2b)** on core 0: GOT loads executed are 0.0 to 0.2% of core 0 in every game, and at most 1.4%
  (micropython) anywhere. The plan's figure was for Quake III's workers, which this profile did not count. Worth doing
  only if it is free; check the workers first.
- **`/dev/zero` a megabyte a step**: neither `read_zero` nor `clear_user` is among any part's top kernel functions.
- **`get_user`/`put_user` byte paths**: the kernel's byte stores are in `__memcpy` (item 4 above), `load_flat_binary`
  and `se_con_put`, not in the user-access helpers.
- **Red Alert's `CellRedraw` clear** (section 1): in this window, core 0 ends 0.3 passes full a guest second and its
  copies are 3.0% of instructions. The gain the plan cites was in another scene. Worth porting, but small here.
- **No fault-in of a write's buffer, no BLAKE2s self-test**: not in any part's top functions (boot-only, outside every
  window).

## The kernel across all parts

The mean share of core 0 per part, over the 51 parts:

| kernel function | core 0 instructions (mean %) | kernel function | core 0 write-cache entries (mean %) |
|---|---|---|---|
| free_pcppages_bulk | 3.31 | handle_exception | 4.81 |
| alloc_pages_run | 2.80 | timekeeping_advance | 2.50 |
| __free_one_page | 1.82 | __memcpy | 2.36 |
| load_flat_binary | 1.65 | timekeeping_update | 2.27 |
| handle_exception | 1.62 | __free_one_page | 2.26 |
| timekeeping_advance | 1.35 | alloc_pages_run | 2.23 |
| __memcpy | 1.13 | free_unref_page_commit | 1.45 |
| free_unref_page | 0.77 | free_page_series | 1.30 |
| free_unref_page_commit | 0.62 | __memset | 1.11 |
| timekeeping_update | 0.61 | tick_do_update_jiffies64 | 0.85 |
| calc_reloc | 0.54 | load_flat_binary | 0.84 |
| ktime_get | 0.42 | rmqueue_bulk | 0.58 |

## Summary across the parts

| part | guest s profiled (s) | core 0 (M instructions / guest s) | all cores (M instructions / guest s) | core 0 passes ended full (/ guest s) | workers' passes ended full (/ guest s) | core 0 texels new a busy pass (texels) | top three of core 0 (% of core 0) | libc copy/str (%) | libc other (%) | libm + soft float (%) | kernel (%) | window server (%) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| doom | 51.8 | 3.02 | 6.7 | 17.1 | 151.7 | 168 | doom:P_CheckPosition 12.4; doom:P_RunThinkers 10.4; doom:R_PointInSubsector 5.4 | 0.0 | 0.0 | 0.0 | 11.9 | 0.1 |
| doom-attract | 12.3 | 2.13 | 2.9 | 32.2 | 34.7 | 48 | kernel:handle_exception 7.1; doom:sight_walk.isra.0 5.6; doom:R_RenderPlayerView 5.3 | 3.4 | 5.3 | 3.4 | 32.0 | 0.0 |
| quake | 20.2 | 2.07 | 7.5 | 1.7 | 38.6 | 133 | quake:SE_SceneFrame 9.5; quake:heard 4.3; quake:__adddf3 4.0 | 4.7 | 6.1 | 9.9 | 10.6 | 0.0 |
| quake-server | 119.5 | 1.90 | 7.6 | 0.6 | 38.6 | 128 | quake:SE_SceneFrame 10.5; quake:heard 4.7; quake:__adddf3 4.4 | 3.5 | 2.4 | 10.9 | 9.1 | 0.0 |
| quake-timedemo | 39.1 | 2.88 | 5.0 | 107.2 | 119.9 | 195 | quake:memcpy 8.8; kernel:__memcpy 7.6; quake:AllocBlock 7.5 | 13.1 | 1.8 | 2.3 | 19.9 | 0.2 |
| quake3 | 8.9 | 1.76 | 11.6 | 1.2 | 90.1 | 103 | ?0x82471000 8.8; quake3:CL_ParsePacketEntities 5.8; quake3:RE_StretchPic 4.8 | 2.3 | 1.6 | 6.6 | 14.6 | 0 |
| quake3-server | 56.6 | 1.86 | 11.6 | 6.4 | 119.9 | 107 | ?0x82471000 8.3; quake3:CL_ParsePacketEntities 6.2; ?0x82470000 4.3 | 2.4 | 6.6 | 6.4 | 18.3 | 0 |
| quake3-menu | 20.3 | 3.30 | 6.6 | 0.0 | 0.0 | 114 | quake3:RE_StretchPic 25.4; ?0x823ac000 12.4; quake3:CL_UISystemCalls 12.2 | 1.5 | 0.5 | 3.7 | 5.6 | 0.0 |
| halflife | 12.1 | 3.21 | 23.7 | 1.7 | 92.5 | 290 | halflife:memcpy 8.6; halflife:PM_PlayerTraceExt 6.8; halflife:CL_RunUsercmd 5.4 | 12.2 | 0.8 | 9.6 | 5.6 | 0 |
| classicube | 75.5 | 2.08 | 11.9 | 0.0 | 788.3 | 103 | ?0x816ed000 14.5; ?0x816bf000 11.9; ?0x816fc000 8.4 | 0.2 | 0.9 | 1.2 | 11.7 | 0.0 |
| tdawn-menu | 40.3 | 3.32 | 3.3 | 1.1 | 0.0 | 40 | tdawn:host_event 34.0; tdawn:_ZN24WWKeyboardClassShaderEmu23Fill_Buffer_From_SystemEv 21.6; tdawn:host_ms 15.9 | 3.3 | 2.6 | 0.0 | 11.1 | 0.2 |
| tdawn | 44.9 | 3.28 | 6.0 | 16.5 | 27.9 | 151 | tdawn:_ZN13InfantryClass2AIEv 10.3; tdawn:_ZN10LogicClass2AIEv 9.8; tdawn:_ZNK11TechnoClass15Greatest_ThreatE10ThreatType 6.9 | 8.8 | 4.8 | 0.0 | 11.4 | 0.2 |
| ralert | 25.1 | 3.29 | 7.6 | 0.3 | 8.5 | 112 | ralert:_ZN13BuildingClass2AIEv 13.8; ralert:_ZN10LogicClass2AIEv 12.8; ralert:_ZN13InfantryClass2AIEv 7.6 | 3.9 | 0.7 | 0.0 | 5.0 | 0.0 |
| nes | 8.2 | 3.32 | 3.3 | 4.6 | 0.0 | 121 | nes:ppu_readhigh 11.7; ?0x81659000 9.0; nes:nes_renderframe.constprop.0 8.5 | 2.5 | 0.6 | 0.2 | 9.5 | 1.5 |
| nxphys | 4.5 | 1.13 | 139.9 | 13.0 | 147.4 | 56 | ?0x81490000 13.0; ?0x8148f000 12.5; kernel:alloc_pages_run 12.0 | 0.2 | 0.2 | 0.0 | 41.5 | 6.5 |
| pinball | 16.8 | 3.32 | 3.3 | 1.0 | 0.0 | 108 | nxpinball:tick 15.7; nano-X:gpu_drawhorzline 11.1; nxpinball:__muldf3 9.5 | 0.1 | 0.4 | 19.9 | 14.4 | 42.6 |
| micropolis | 86.6 | 3.32 | 3.3 | 0.4 | 0.0 | 56 | nxmicropolis:_ZN10Micropolis12animateTilesEv 39.4; nxmicropolis:_ZN10Micropolis7mapScanEii 7.7; nxmicropolis:_ZN10Micropolis17testForConductiveERK8Position10Direction2 7.1 | 0.0 | 0.0 | 0.0 | 4.4 | 0 |
| openttd | 55.1 | 2.73 | 13.1 | 77.3 | 20.6 | 163 | openttd:memcpy_any 19.0; openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh 16.3; openttd:_ZN21Blitter_8bppOptimized6EncodeE10SpriteTypeRK13SpriteCollMapIN12SpriteLoader6SpriteEER15SpriteAllocator 14.8 | 3.7 | 19.7 | 0.9 | 8.6 | 0 |
| openttd-title | 20.3 | 3.31 | 3.3 | 19.7 | 0.0 | 122 | openttd:_ZN20MissingGlyphSearcher17FindMissingGlyphsEv 8.9; openttd:_Z19InitializeLandscapev 5.6; openttd:_ZN15SpriteFontCache14MapCharToGlyphEDib 5.4 | 3.4 | 8.3 | 0.1 | 10.6 | 0.0 |
| video-mpg | 20.3 | 2.74 | 33.3 | 1.9 | 990.5 | 65 | ?0x819d6000 33.6; kernel:handle_exception 7.2; ?0x819ce000 6.0 | 0.0 | 0.0 | 0.0 | 24.0 | 5.8 |
| video-avi | 6.0 | 2.43 | 60.4 | 6.3 | 1876.6 | 66 | ?0x8167c000 10.6; kernel:handle_exception 9.2; ?0x8167d000 8.8 | 0.1 | 0.1 | 0.0 | 41.4 | 8.3 |
| decode-avi | 62.1 | 2.96 | 8.6 | 0.1 | 190.1 | 67 | media_test:main 50.1; media_test:media_sum 38.3; ?0x816b6000 3.2 | 0.0 | 0.0 | 0.0 | 6.4 | 0 |
| nxray | 2.4 | 1.37 | 463.5 | 20.4 | 0.0 | 154 | nxray:main 27.4; ?0x8148f000 12.6; kernel:handle_exception 6.2 | 0.1 | 0.2 | 0.0 | 40.8 | 8.2 |
| nxpath | 4.3 | 3.12 | 17.6 | 39.6 | 87.0 | 163 | nxpath:sc_mesh_load 25.4; nxpath:show_mesh 12.3; nxpath:number.constprop.0 11.6 | 4.8 | 2.0 | 1.3 | 16.4 | 4.1 |
| nxweb | 0.6 | 1.98 | 2.1 | 16.5 | 0.0 | 134 | kernel:alloc_pages_run 14.1; nxweb:web_parse 7.7; nxweb:web_put_word 3.7 | 5.5 | 7.3 | 0.0 | 51.9 | 3.7 |
| nxedit | 20.2 | 0.28 | 0.3 | 0.3 | 0.0 | 134 | kernel:timekeeping_advance 6.5; nano-X:gpu_drawstring 3.0; kernel:ktime_get 3.0 | 4.0 | 4.9 | 0.0 | 71.4 | 7.9 |
| nxide | 4.1 | 3.29 | 3.3 | 26.1 | 0.0 | 130 | nxide:printf_core 8.3; ?0x816e1000 2.9; nxide:memcpy_any 2.7 | 4.1 | 16.5 | 3.6 | 27.8 | 7.0 |
| desktop | 14.7 | 1.82 | 4.5 | 7.4 | 0.0 | 163 | kernel:free_pcppages_bulk 10.7; kernel:__free_one_page 5.8; kernel:alloc_pages_run 5.8 | 4.7 | 6.9 | 0.0 | 70.9 | 7.7 |
| nxview | 1.8 | 2.80 | 126.4 | 27.2 | 0.0 | 109 | kernel:free_pcppages_bulk 12.8; kernel:__free_one_page 10.7; nxview:__udivmoddi4 8.2 | 1.3 | 1.5 | 9.9 | 60.6 | 6.6 |
| nxpaint | 1.2 | 2.81 | 4.4 | 47.7 | 710.2 | 132 | nxpaint:draw_tools 16.4; nxpaint:scripted 11.5; kernel:alloc_pages_run 6.7 | 1.9 | 3.4 | 0.1 | 29.2 | 17.6 |
| nxfiles | 20.2 | 0.24 | 0.2 | 0.2 | 0.0 | 123 | kernel:timekeeping_advance 7.6; nxfiles:strcasecmp 3.9; ?0x8194e000 3.6 | 7.3 | 2.7 | 0.0 | 77.3 | 6.7 |
| memory | 29.9 | 2.14 | 2.1 | 199.4 | 0.0 | 227 | ?0x81478000 42.7; ?0x81483000 10.2; ?0x81477000 8.4 | 8.2 | 1.8 | 0.4 | 15.6 | 0.2 |
| memory-kernel | 6.5 | 1.84 | 1.8 | 0.9 | 0.0 | 73 | kernel:copy_page_to_iter 8.4; kernel:filemap_read 5.4; kernel:free_pcppages_bulk 4.7 | 0.3 | 0.2 | 0.0 | 95.8 | 1.0 |
| cc | 7.9 | 3.29 | 3.3 | 37.3 | 0.0 | 130 | tcc:next_nomacro 10.4; tcc:strcmp 8.5; tcc:tcc_load_object_file 4.8 | 16.9 | 7.9 | 0.3 | 21.5 | 0 |
| qjs | 1.2 | 3.31 | 3.3 | 12.9 | 0.0 | 122 | qjs:__udivdi3 5.1; qjs:__umoddi3 4.7; qjs:js_bigint_divrem 3.5 | 4.0 | 3.9 | 11.4 | 19.7 | 0.2 |
| python | 7.2 | 3.32 | 3.3 | 0.6 | 0.0 | 58 | micropython:mp_execute_bytecode 21.5; micropython:gc_alloc 12.8; micropython:mp_binary_op 8.5 | 6.0 | 0.1 | 0.0 | 7.6 | 0 |
| gzip | 65.0 | 3.32 | 3.3 | 0.8 | 0.0 | 53 | busybox:deflate 47.3; busybox:__multf3 7.8; busybox:fmt_fp 5.2 | 0.7 | 14.0 | 18.3 | 5.7 | 0.1 |
| htop | 2.7 | 3.12 | 3.1 | 13.2 | 0.0 | 152 | htop:memcmp 14.6; htop:memset 4.7; htop:printf_core 2.3 | 23.3 | 16.2 | 1.7 | 45.9 | 0.1 |
| tetris | 21.7 | 1.46 | 3.3 | 5.1 | 0.0 | 157 | kernel:free_pcppages_bulk 10.4; kernel:alloc_pages_run 5.6; kernel:__free_one_page 5.6 | 2.8 | 9.6 | 0.1 | 71.3 | 8.6 |
| mines | 21.8 | 1.49 | 3.3 | 5.0 | 0.0 | 156 | kernel:free_pcppages_bulk 10.2; kernel:alloc_pages_run 5.6; kernel:__free_one_page 5.5 | 3.3 | 7.8 | 0.1 | 69.8 | 11.7 |
| terminal | 21.9 | 1.48 | 3.3 | 5.3 | 0.0 | 160 | kernel:free_pcppages_bulk 9.9; kernel:alloc_pages_run 5.8; kernel:__free_one_page 5.5 | 4.0 | 7.8 | 0.1 | 71.2 | 8.9 |
| clock | 22.4 | 1.55 | 3.3 | 5.0 | 0.0 | 153 | kernel:free_pcppages_bulk 9.6; kernel:alloc_pages_run 5.3; kernel:__free_one_page 5.2 | 2.9 | 8.8 | 0.1 | 66.7 | 14.4 |
| monitor | 21.9 | 1.60 | 4.3 | 5.5 | 1.1 | 155 | kernel:free_pcppages_bulk 9.4; kernel:alloc_pages_run 5.2; kernel:__free_one_page 5.0 | 4.3 | 10.5 | 0.3 | 68.7 | 8.8 |
| network | 21.7 | 1.46 | 3.3 | 5.4 | 0.0 | 156 | kernel:free_pcppages_bulk 10.4; kernel:alloc_pages_run 5.6; kernel:__free_one_page 5.6 | 3.3 | 9.6 | 0.1 | 71.2 | 8.2 |
| settings | 21.7 | 1.50 | 3.3 | 5.1 | 0.0 | 154 | kernel:free_pcppages_bulk 10.1; kernel:alloc_pages_run 5.5; kernel:__free_one_page 5.4 | 4.0 | 7.5 | 0.1 | 70.2 | 5.8 |
| eyes | 21.8 | 1.47 | 3.3 | 4.8 | 0.0 | 158 | kernel:free_pcppages_bulk 10.3; kernel:alloc_pages_run 5.6; kernel:__free_one_page 5.5 | 3.9 | 6.9 | 0.1 | 70.7 | 9.6 |
| glxgears | 22.6 | 2.64 | 4.4 | 6.3 | 0.0 | 146 | ?0x818d1000 22.0; ?0x818d2000 11.2; kernel:free_pcppages_bulk 5.5 | 0.9 | 3.4 | 3.1 | 38.3 | 6.0 |
| imgui | 22.6 | 2.64 | 5.0 | 8.2 | 22.7 | 191 | imdemo:_ZN10ImDrawList11AddPolylineEPK6ImVec2ijif.part.0 9.8; imdemo:_ZN6ImFont10RenderTextEP10ImDrawListfRK6ImVec2jRK6ImVec4PKcS9_fb.isra.0 9.6; kernel:free_pcppages_bulk 5.4 | 1.8 | 4.4 | 0.0 | 38.5 | 3.8 |
| music | 22.6 | 2.64 | 4.4 | 5.2 | 0.0 | 105 | nxplay:__adddf3 15.8; nxplay:__muldf3 12.8; kernel:free_pcppages_bulk 5.5 | 2.8 | 5.9 | 32.9 | 39.0 | 5.5 |
| nes-nova | 22.6 | 1.87 | 4.0 | 6.6 | 1.2 | 52 | kernel:free_pcppages_bulk 7.7; kernel:handle_exception 6.1; kernel:alloc_pages_run 5.1 | 2.1 | 7.3 | 0.2 | 69.4 | 6.2 |
| example-cube | 21.9 | 1.49 | 3.3 | 5.5 | 0.0 | 155 | kernel:free_pcppages_bulk 10.1; kernel:alloc_pages_run 5.5; kernel:__free_one_page 5.4 | 4.3 | 10.1 | 0.1 | 70.7 | 6.6 |

## The parts

### doom

Doom: timedemo demo1 (as fast as it goes). `DOOM_HASH_TICS=100 doom -1 -timedemo demo1`

Window: `the command` to `timed `: 51.8 s of the guest, 25,164 passes; core 0 156.6 M instructions (3.02 M a guest second, 6,224 a pass), all cores 348.2 M, 6 cores ran. Core 0 took 3,469,600 write-cache entries (168 distinct texels a pass that stored), and 888 of its passes ended full (17.1 a guest second); the workers' 7,868 (151.7 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 79.1 | 78.3 | 95.5 |
| kernel | 11.9 | 12.9 | 0 |
| window server | 0.1 | 0.1 | 0 |
| unknown | 8.9 | 8.7 | 4.5 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| doom:P_CheckPosition | 12.4 | doom:P_RunThinkers | 19.1 |
| doom:P_RunThinkers | 10.4 | doom:R_RenderPlayerView | 10.3 |
| doom:R_PointInSubsector | 5.4 | doom:P_CheckPosition | 7.8 |
| doom:R_RenderPlayerView | 5.0 | doom:P_SetMobjState.isra.0 | 5.8 |
| doom:P_SetMobjState.isra.0 | 3.2 | kernel:__memcpy | 3.7 |
| doom:P_LookForPlayers | 3.1 | doom:swap | 2.4 |
| doom:sight_walk.isra.0 | 2.8 | doom:T_Glow | 2.3 |
| kernel:__memcpy | 2.7 | ?0x8169a000 | 2.1 |

Core 0's passes ended full, where (passes): kernel:__memcpy 211, ?0x81699000 104, doom:P_RunThinkers 84, ?0x8169a000 81, doom:R_RenderPlayerView 58.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| doom:world_things | 47.3 | doom:world_things | 34.6 |
| doom:sight_walk.isra.0 | 8.5 | doom:sector_relit | 7.4 |
| doom:things_told | 5.6 | doom:seglKeptGreyIn | 6.4 |
| doom:doom_view_draw | 5.1 | doom:doom_view_draw | 5.6 |
| doom:seglKeptGreyIn | 4.4 | doom:things_told | 5.6 |
| doom:seglQuad | 3.6 | doom:seglQuad | 4.5 |

The workers' passes ended full, where (passes): doom:world_things 2834, doom:sector_relit 783, doom:seglKeptGreyIn 694, doom:seglQuad 592, doom:seglKeptTexture 569.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 1,182 (0.0% of core 0, 189 places), doom 182,635 (0.12% of core 0, 116 places).

Slow forms prof.py finds (% of core 0): 7.5%  [independent-pixel-loop] doom:P_RunThinkers+0x214  (479,485 trips of 25).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.01% of core 0's instructions and 0.01% of its write-cache entries, calls of about 2 KB or more 0.0% (doom:memcpy 117 calls of 59 instructions; doom:memset 56 calls of 62 instructions; nano-X:memset 7 calls of 86 instructions). The kernel's `__memcpy` 2.69% (byte paths 2.06%, 64-byte word loop 0.5%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| doom:G_InitNew | 1 | 2,139,856 | 2,139,856 |
| doom:split.constprop.0 | 1 | 1,252,502 | 1,252,502 |
| doom:S_ChangeMusic | 1 | 1,174,094 | 1,174,094 |
| doom:R_InitTextures | 1 | 758,062 | 758,062 |
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| doom:split | 2 | 624,640 | 825,700 |

### doom-attract

Doom: the title and its demos as a player sees them (35 tics a second). `doom -1`

Window: 5 s after the command to 12 s later: 12.3 s of the guest, 18,422 passes; core 0 26.2 M instructions (2.13 M a guest second, 1,422 a pass), all cores 35.4 M, 6 cores ran. Core 0 took 552,904 write-cache entries (48 distinct texels a pass that stored), and 395 of its passes ended full (32.2 a guest second); the workers' 426 (34.7 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 55.0 | 48.2 | 96.1 |
| libc copy/fill/str | 3.4 | 14.5 | 1.5 |
| libc other | 5.3 | 5.2 | 0 |
| soft float, 64-bit | 3.4 | 0.4 | 0 |
| kernel | 32.0 | 31.1 | 0 |
| unknown | 0.9 | 0.7 | 2.4 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:handle_exception | 7.1 | kernel:handle_exception | 8.7 |
| doom:sight_walk.isra.0 | 5.6 | doom:memset | 8.1 |
| doom:R_RenderPlayerView | 5.3 | doom:R_RenderPlayerView | 7.7 |
| kernel:__memcpy | 4.7 | kernel:__memcpy | 6.8 |
| doom:memcpy_any | 4.6 | doom:P_RunThinkers | 6.6 |
| kernel:sys_ioctl | 3.3 | doom:memcpy | 6.3 |
| doom:seglQuad | 3.2 | doom:memcpy_any | 4.7 |
| doom:I_GetTime | 3.1 | doom:NetUpdate | 3.2 |

Core 0's passes ended full, where (passes): doom:memset 91, doom:memcpy 75, kernel:__memcpy 65, doom:R_RenderPlayerView 42, doom:G_DoLoadLevel 37.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| doom:world_things | 44.5 | doom:world_things | 36.0 |
| doom:sight_walk.isra.0 | 14.1 | doom:sector_relit | 6.8 |
| doom:things_told | 4.6 | doom:sight_walk.isra.0 | 6.7 |
| doom:doom_view_draw | 4.1 | doom:seglKeptGreyIn | 5.9 |
| doom:seglKeptGreyIn | 3.8 | doom:seglQuad | 5.0 |
| doom:seglSettle | 3.5 | doom:things_told | 4.8 |

The workers' passes ended full, where (passes): doom:world_things 162, doom:seglKeptGreyIn 79, doom:sector_relit 63, doom:seglKeptGrey 22, doom:seglKeptTexture 22.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): doom 16,634 (0.06% of core 0, 106 places).

Slow forms prof.py finds (% of core 0): 3.6%  [byte-copy] vmlinux:__memcpy+0x17c  (190,030 trips of 5).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.38% of core 0's instructions and 14.45% of its write-cache entries, calls of about 2 KB or more 0.87% (doom:memcpy 17,048 calls of 94 instructions; doom:memset 1,063 calls of 202 instructions; doom:memset >=2KB 45 calls of 3,461 instructions). The kernel's `__memcpy` 4.67% (byte paths 3.63%, 64-byte word loop 0.85%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): doom:split 3.36, doom:__divdi3 3.19.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): doom:wall_texture 36 0.44, doom:G_InitNew 9 0.16.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| doom:D_DoomMain | 1 | 4,284,478 | 4,284,478 |
| doom:D_DoomLoop | 1 | 1,902,686 | 1,902,686 |
| doom:G_InitNew | 1 | 1,552,224 | 1,552,224 |
| doom:split.constprop.0 | 1 | 1,252,502 | 1,252,502 |
| doom:R_InitTextures | 1 | 758,000 | 758,000 |
| doom:split | 2 | 624,640 | 825,700 |

### quake

Quake: e1m1 loaded and played (standing). `QUAKE_TRACK=5 quake +map e1m1`

Window: `track 10:` to `track 1200:`: 20.2 s of the guest, 8,436 passes; core 0 41.9 M instructions (2.07 M a guest second, 4,968 a pass), all cores 151.9 M, 14 cores ran. Core 0 took 442,280 write-cache entries (133 distinct texels a pass that stored), and 34 of its passes ended full (1.7 a guest second); the workers' 781 (38.6 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 58.7 | 61.0 | 95.7 |
| libc copy/fill/str | 4.7 | 2.6 | 2.3 |
| libc other | 6.1 | 6.4 | 0.0 |
| libm | 0.4 | 1.1 | 0.1 |
| soft float, 64-bit | 9.5 | 1.6 | 0 |
| kernel | 10.6 | 20.3 | 0 |
| window client | 0.0 | 0.1 | 0 |
| unknown | 9.9 | 6.8 | 1.7 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| quake:SE_SceneFrame | 9.5 | quake:SE_SceneFrame | 7.1 |
| quake:heard | 4.3 | quake:seglSwapTake | 4.7 |
| quake:__adddf3 | 4.0 | quake:_Host_Frame | 3.9 |
| ?0x81629000 | 3.8 | quake:qglText | 2.9 |
| quake:q_sin | 3.1 | quake:SE_ParticlesGive | 2.7 |
| ?0x81657000 | 2.7 | kernel:handle_exception | 2.5 |
| quake:__muldf3 | 2.7 | quake:mcw_done | 2.5 |
| quake:CL_RelinkEntities | 2.0 | kernel:timekeeping_advance | 2.4 |

Core 0's passes ended full, where (passes): quake:qglText 29, quake:swap 1, ?0x8168f000 1, quake:qglSwap 1, kernel:handle_exception 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| quake:light_share | 20.4 | quake:light_share | 18.4 |
| quake:PR_ExecuteProgram | 7.5 | quake:SV_ClipMoveToEntity | 8.1 |
| quake:SV_Physics | 5.7 | quake:PR_ExecuteProgram | 5.0 |
| quake:SV_WriteEntitiesToClient | 5.0 | quake:scene_job | 4.5 |
| quake:SV_PointContents | 3.6 | quake:SV_Move | 4.4 |
| quake:SE_NodeCheck | 3.2 | quake:PR_EnterFunction | 2.9 |

The workers' passes ended full, where (passes): quake:light_share 780, quake:qglModel 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): quake 1,886 (0.0% of core 0, 2 places).

Slow forms prof.py finds (% of core 0): 3.9%  [printf-in-hot-path] quake:vfprintf  (271 calls of 5,971); 3.7%  [printf-in-hot-path] quake:printf_core  (542 calls of 2,893); 3.6%  [software-float] quake:__adddf3  (9,410 calls of 159); 3.5%  [printf-in-hot-path] quake:vsnprintf  (240 calls of 6,186); 2.7%  [software-float] quake:__muldf3  (11,292 calls of 100).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 1.8% of core 0's instructions and 2.34% of its write-cache entries, calls of about 2 KB or more 0.0% (quake:memcpy 14,270 calls of 41 instructions; quake:memset 1,136 calls of 33 instructions). The kernel's `__memcpy` 0.49% (byte paths 0.37%, 64-byte word loop 0.01%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): quake:logf 7.37, quake:Sys_FloatTime 1.38.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| quake:CL_ParseServerInfo | 1 | 28,270,183 | 28,270,183 |
| quake:R_NewMap | 1 | 27,580,003 | 27,580,003 |
| quake:SV_SpawnServer | 1 | 27,200,925 | 27,200,925 |
| quake:ED_LoadFromFile | 1 | 15,184,714 | 15,184,714 |
| quake:PR_LoadProgs | 1 | 4,587,805 | 4,587,805 |
| quake:CRC_ProcessBlock | 1 | 3,924,672 | 3,924,672 |

### quake-server

Quake: e1m1, the server's ticks on its worker core (quakestat: every 2 s). `QUAKE_STATS_MS=2000 quake +map e1m1`

Window: `a level's first frame` to `the wall (50 s of the host)`: 119.5 s of the guest, 49,952 passes; core 0 227.0 M instructions (1.90 M a guest second, 4,545 a pass), all cores 903.2 M, 14 cores ran. Core 0 took 2,371,811 write-cache entries (128 distinct texels a pass that stored), and 70 of its passes ended full (0.6 a guest second); the workers' 4,610 (38.6 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 63.2 | 63.8 | 95.8 |
| libc copy/fill/str | 3.5 | 2.1 | 2.3 |
| libc other | 2.4 | 4.9 | 0.0 |
| libm | 0.5 | 1.4 | 0.1 |
| soft float, 64-bit | 10.4 | 1.8 | 0 |
| kernel | 9.1 | 18.7 | 0 |
| window client | 0.0 | 0.1 | 0 |
| unknown | 11.0 | 7.2 | 1.7 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| quake:SE_SceneFrame | 10.5 | quake:SE_SceneFrame | 7.9 |
| quake:heard | 4.7 | quake:seglSwapTake | 5.3 |
| quake:__adddf3 | 4.4 | quake:_Host_Frame | 4.5 |
| ?0x81629000 | 4.2 | quake:SE_ParticlesGive | 3.0 |
| quake:q_sin | 3.5 | quake:mcw_done | 3.0 |
| ?0x81657000 | 3.1 | kernel:timekeeping_advance | 2.7 |
| quake:__muldf3 | 2.9 | quake:mcw_post | 2.7 |
| quake:CL_RelinkEntities | 2.2 | ?0x8165a000 | 2.5 |

Core 0's passes ended full, where (passes): quake:qglText 61, quake:memcpy 2, quake:fps_caption 2, quake:shade_flush_all 2, quake:seglSwapTake 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| quake:light_share | 19.5 | quake:light_share | 17.7 |
| quake:PR_ExecuteProgram | 7.4 | quake:SV_ClipMoveToEntity | 8.7 |
| quake:SV_Physics | 5.8 | quake:PR_ExecuteProgram | 4.9 |
| quake:SV_WriteEntitiesToClient | 5.1 | quake:SV_Move | 4.4 |
| quake:SE_NodeCheck | 4.5 | quake:scene_job | 4.4 |
| quake:SV_PointContents | 3.4 | quake:PR_EnterFunction | 2.9 |

The workers' passes ended full, where (passes): quake:light_share 4610.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): quake 11,208 (0.0% of core 0, 2 places).

Slow forms prof.py finds (% of core 0): 3.9%  [software-float] quake:__adddf3  (55,740 calls of 159); 2.9%  [software-float] quake:__muldf3  (66,888 calls of 100); 1.5%  [software-float] quake:__divdf3  (18,850 calls of 176).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 1.67% of core 0's instructions and 2.03% of its write-cache entries, calls of about 2 KB or more 0.0% (quake:memcpy 25,824 calls of 62 instructions; quake:memset 1,666 calls of 33 instructions). The kernel's `__memcpy` 0.16% (byte paths 0.1%, 64-byte word loop 0.02%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): quake:logf 8.06, quake:Sys_FloatTime 1.46.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| quake:CL_ParseServerInfo | 1 | 28,270,245 | 28,270,245 |
| quake:R_NewMap | 1 | 27,580,065 | 27,580,065 |
| quake:SV_SpawnServer | 1 | 27,202,340 | 27,202,340 |
| quake:ED_LoadFromFile | 1 | 15,186,130 | 15,186,130 |
| quake:PR_LoadProgs | 1 | 4,587,805 | 4,587,805 |
| quake:CRC_ProcessBlock | 1 | 3,924,672 | 3,924,672 |

### quake-timedemo

Quake: timedemo demo1. `quake +timedemo demo1`

Window: `the command` to ` fps`: 39.1 s of the guest, 18,269 passes; core 0 112.8 M instructions (2.88 M a guest second, 6,172 a pass), all cores 196.0 M, 14 cores ran. Core 0 took 2,393,382 write-cache entries (195 distinct texels a pass that stored), and 4,194 of its passes ended full (107.2 a guest second); the workers' 4,691 (119.9 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 61.0 | 35.0 | 93.1 |
| libc copy/fill/str | 13.1 | 44.8 | 2.0 |
| libc other | 1.8 | 1.3 | 0.0 |
| libm | 0.7 | 0.1 | 0.2 |
| soft float, 64-bit | 1.6 | 0.2 | 0 |
| kernel | 19.9 | 18.0 | 0 |
| window server | 0.2 | 0.1 | 0 |
| unknown | 1.7 | 0.6 | 4.7 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| quake:memcpy | 8.8 | quake:memcpy | 35.3 |
| kernel:__memcpy | 7.6 | quake:memset | 9.0 |
| quake:AllocBlock | 7.5 | kernel:__memcpy | 7.6 |
| quake:CL_ParseUpdate | 4.5 | quake:GL_MakeAliasModelDisplayLists | 7.0 |
| quake:heard | 4.4 | quake:qglLight | 2.4 |
| quake:BuildSurfaceDisplayList | 4.0 | quake:SE_SceneFrame | 2.0 |
| quake:GL_MakeAliasModelDisplayLists | 3.4 | quake:CL_ParseUpdate | 1.7 |
| quake:CL_RelinkEntities | 3.4 | quake:put_quads | 1.6 |

Core 0's passes ended full, where (passes): quake:memcpy 2542, quake:GL_MakeAliasModelDisplayLists 503, quake:memset 279, kernel:__memcpy 257, quake:put_quads 118.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| quake:Mod_FloodFillSkin | 16.1 | quake:SE_ParticlesJob | 16.7 |
| quake:rec_rect | 8.7 | quake:Mod_FloodFillSkin | 16.3 |
| quake:water_warp.isra.0 | 8.6 | quake:water_warp.isra.0 | 13.7 |
| quake:RecursiveLightPoint.part.0 | 7.8 | quake:memcpy | 6.3 |
| quake:SE_ParticlesJob | 6.7 | quake:qgpuRows | 3.6 |
| quake:light_share | 5.1 | quake:qgpuReplay | 3.4 |

The workers' passes ended full, where (passes): quake:SE_ParticlesJob 1461, quake:water_warp.isra.0 922, quake:Mod_FloodFillSkin 565, quake:memcpy 487, quake:compact_begin 112.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 1,195 (0.0% of core 0, 158 places).

Slow forms prof.py finds (% of core 0): 7.6%  [independent-pixel-loop] quake:memcpy+0x20  (245,365 trips of 35); 6.7%  [byte-copy] vmlinux:__memcpy+0x17c  (1,520,618 trips of 5).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 12.09% of core 0's instructions and 44.63% of its write-cache entries, calls of about 2 KB or more 5.64% (quake:memcpy >=2KB 442 calls of 11,363 instructions; quake:memcpy 41,992 calls of 64 instructions; quake:memset 17,633 calls of 103 instructions). The kernel's `__memcpy` 7.62% (byte paths 6.74%, 64-byte word loop 0.69%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): quake:logf 1.41.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): quake:Hunk_AllocName 607 1.11.

### quake3

Quake III: q3dm17 with a bot. `QUAKE3_STATS_FRAMES=100 quake3 +set com_maxfps 0 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3`

Window: `quake3: the level's first frame` to `q3stat: frame 600:`: 8.9 s of the guest, 3,626 passes; core 0 15.7 M instructions (1.76 M a guest second, 4,328 a pass), all cores 103.3 M, 32 cores ran. Core 0 took 204,892 write-cache entries (103 distinct texels a pass that stored), and 11 of its passes ended full (1.2 a guest second); the workers' 805 (90.1 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 54.8 | 53.0 | 93.9 |
| libc copy/fill/str | 2.3 | 6.8 | 4.5 |
| libc other | 1.6 | 0.7 | 0.2 |
| libm | 0.3 | 0.3 | 0.1 |
| soft float, 64-bit | 6.3 | 1.0 | 0.0 |
| kernel | 14.6 | 24.6 | 0 |
| unknown | 20.0 | 13.6 | 1.2 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x82471000 | 8.8 | quake3:CL_ParsePacketEntities | 15.1 |
| quake3:CL_ParsePacketEntities | 5.8 | kernel:handle_exception | 7.1 |
| quake3:RE_StretchPic | 4.8 | ?0x82470000 | 4.1 |
| ?0x82470000 | 4.4 | quake3:memcpy | 3.8 |
| ?0x82475000 | 4.3 | ?0x82471000 | 3.5 |
| quake3:Con_DrawNotify | 3.4 | quake3:memset | 3.0 |
| quake3:heard | 3.3 | ?0x82475000 | 2.7 |
| quake3:__adddf3 | 3.2 | quake3:SE_Phase | 2.5 |

Core 0's passes ended full, where (passes): quake3:CL_ParsePacketEntities 3, quake3:CL_ParseSnapshot 3, quake3:SE_Phase 2, quake3:memcpy 2, quake3:CL_GetSnapshot 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| quake3:entity_draw | 7.1 | quake3:gpuDrawAt | 8.1 |
| quake3:gpuDrawRoom | 4.3 | quake3:SE_AddRefEntity | 7.7 |
| quake3:AAS_TraceClientBBox | 3.9 | quake3:stage_moved | 7.3 |
| quake3:picture_draw | 3.3 | quake3:memset | 6.5 |
| quake3:light_at | 3.3 | quake3:SV_SendClientSnapshot | 5.1 |
| quake3:CG_Item | 3.0 | quake3:entity_matrix | 4.7 |

The workers' passes ended full, where (passes): quake3:SV_SendClientSnapshot 220, quake3:CG_ProcessSnapshots 64, quake3:CM_TraceThroughLeaf 51, quake3:CG_PredictPlayerState 35, quake3:G_TouchTriggers 35.

Slow forms prof.py finds (% of core 0): 2.9%  [software-float] quake3:__adddf3  (2,860 calls of 157); 2.2%  [software-float] quake3:__muldf3  (3,432 calls of 100).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.32% of core 0's instructions and 6.84% of its write-cache entries, calls of about 2 KB or more 0.0% (quake3:memcpy 3,881 calls of 53 instructions; quake3:memset 3,313 calls of 57 instructions). The kernel's `__memcpy` 0.24% (byte paths 0.17%, 64-byte word loop 0.02%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): quake3:logf 5.96.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| quake3:SV_SpawnServer | 1 | 54,585,848 | 54,585,848 |
| quake3:SV_InitGameProgs | 1 | 42,917,624 | 42,917,624 |
| quake3:G_InitGame | 1 | 42,912,524 | 42,912,524 |
| quake3:BotAISetup | 1 | 29,419,949 | 29,419,949 |
| quake3:BotInitLibrary | 1 | 29,402,487 | 29,402,487 |
| quake3:trap_BotLibSetup | 1 | 29,381,565 | 29,381,565 |

### quake3-server

Quake III: the server's ticks on its worker (a q3srv: line a job). `QUAKE3_SERVER_LOG=1 QUAKE3_STATS_FRAMES=100 quake3 +set com_maxfps 0 +set fixedtime 33 +devmap q3dm17 +addbot sarge 3`

Window: `quake3: the level's first frame` to `the wall (50 s of the host)`: 56.6 s of the guest, 23,070 passes; core 0 105.5 M instructions (1.86 M a guest second, 4,576 a pass), all cores 656.7 M, 32 cores ran. Core 0 took 1,458,143 write-cache entries (107 distinct texels a pass that stored), and 365 of its passes ended full (6.4 a guest second); the workers' 6,792 (119.9 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 47.2 | 52.2 | 93.7 |
| libc copy/fill/str | 2.4 | 6.3 | 4.6 |
| libc other | 6.6 | 2.3 | 0.2 |
| libm | 0.3 | 0.3 | 0.1 |
| soft float, 64-bit | 6.1 | 1.0 | 0.0 |
| kernel | 18.3 | 26.1 | 0 |
| unknown | 19.0 | 11.8 | 1.3 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x82471000 | 8.3 | quake3:CL_ParsePacketEntities | 17.6 |
| quake3:CL_ParsePacketEntities | 6.2 | kernel:handle_exception | 6.6 |
| ?0x82470000 | 4.3 | ?0x82470000 | 3.7 |
| ?0x82475000 | 4.1 | quake3:memcpy | 3.4 |
| quake3:heard | 3.4 | ?0x82471000 | 3.0 |
| kernel:n_tty_write | 3.2 | quake3:memset | 2.9 |
| quake3:__adddf3 | 3.2 | quake3:SE_Phase | 2.4 |
| quake3:printf_core | 2.6 | ?0x82475000 | 2.3 |

Core 0's passes ended full, where (passes): quake3:CL_ParsePacketEntities 254, quake3:CL_ParseSnapshot 27, quake3:SE_Phase 11, ?0x82471000 7, quake3:memcpy 6.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| quake3:entity_draw | 7.6 | quake3:SE_AddRefEntity | 9.4 |
| quake3:SE_AddRefEntity | 3.5 | quake3:gpuDrawAt | 7.7 |
| quake3:gpuDrawRoom | 3.4 | quake3:stage_moved | 7.1 |
| quake3:light_at | 3.4 | quake3:memset | 6.0 |
| quake3:CG_Item | 3.2 | quake3:SV_SendClientSnapshot | 5.8 |
| quake3:gpuDrawAt | 3.0 | quake3:entity_matrix | 4.5 |

The workers' passes ended full, where (passes): quake3:SV_SendClientSnapshot 1898, quake3:SE_AddRefEntity 557, quake3:CG_ProcessSnapshots 472, quake3:CG_AddLocalEntities 441, quake3:CM_TraceThroughLeaf 329.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): quake3 1,287 (0.0% of core 0, 3 places).

Slow forms prof.py finds (% of core 0): 6.5%  [printf-in-hot-path] quake3:printf  (873 calls of 7,835); 6.5%  [printf-in-hot-path] quake3:vfprintf  (883 calls of 7,739); 6.4%  [printf-in-hot-path] quake3:printf_core  (1,766 calls of 3,831); 3.6%  [independent-pixel-loop] quake3:CL_ParsePacketEntities+0x628  (345,038 trips of 11); 2.9%  [independent-pixel-loop] vmlinux:n_tty_write+0x360  (146,924 trips of 21).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.42% of core 0's instructions and 6.28% of its write-cache entries, calls of about 2 KB or more 0.02% (quake3:memcpy 56,337 calls of 44 instructions; quake3:memset 21,658 calls of 57 instructions; quake3:memset >=2KB 4 calls of 6,326 instructions). The kernel's `__memcpy` 0.63% (byte paths 0.39%, 64-byte word loop 0.1%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): quake3:logf 5.82.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| quake3:SV_SpawnServer | 1 | 54,585,908 | 54,585,908 |
| quake3:SV_InitGameProgs | 1 | 42,917,619 | 42,917,619 |
| quake3:G_InitGame | 1 | 42,912,519 | 42,912,519 |
| quake3:BotAISetup | 1 | 29,419,943 | 29,419,943 |
| quake3:BotInitLibrary | 1 | 29,402,481 | 29,402,481 |
| quake3:trap_BotLibSetup | 1 | 29,381,560 | 29,381,560 |

### quake3-menu

Quake III: its main menu. `quake3`

Window: 30 s after the command to 20 s later: 20.3 s of the guest, 8,237 passes; core 0 66.9 M instructions (3.30 M a guest second, 8,125 a pass), all cores 133.8 M, 5 cores ran. Core 0 took 351,234 write-cache entries (114 distinct texels a pass that stored), and 1 of its passes ended full (0.0 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 74.1 | 42.1 | 99.2 |
| libc copy/fill/str | 1.5 | 7.2 | 0.7 |
| libc other | 0.5 | 0.4 | 0 |
| libm | 0.2 | 0.1 | 0 |
| soft float, 64-bit | 3.5 | 1.4 | 0 |
| kernel | 5.6 | 23.3 | 0 |
| window client | 0.0 | 0.1 | 0 |
| unknown | 14.7 | 25.3 | 0.1 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| quake3:RE_StretchPic | 25.4 | ?0x823ac000 | 21.2 |
| ?0x823ac000 | 12.4 | quake3:CL_UISystemCalls | 5.9 |
| quake3:CL_UISystemCalls | 12.2 | quake3:memset | 5.7 |
| quake3:UI_DrawProportionalString2 | 11.4 | quake3:UI_DrawProportionalString2 | 4.6 |
| quake3:trap_R_DrawStretchPic | 6.0 | quake3:RE_StretchPic | 3.9 |
| quake3:UI_DrawString2.part.0 | 5.6 | kernel:handle_exception | 3.7 |
| quake3:UI_DrawProportionalString | 2.2 | kernel:timekeeping_advance | 3.0 |
| quake3:RE_SetColor | 1.9 | kernel:timekeeping_update | 2.7 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| quake3:gpuDrawRoom | 44.6 | quake3:gpuDrawRoom | 34.7 |
| quake3:picture_draw | 40.1 | quake3:picture_draw | 13.2 |
| quake3:list_draw | 4.9 | quake3:stage_moved | 12.6 |
| quake3:stage_moved | 2.6 | quake3:gpuDrawAt | 8.5 |
| quake3:entity_draw | 1.2 | quake3:gpuProjection | 8.2 |
| quake3:gpuDrawAt | 1.0 | quake3:gpuModelview | 3.6 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): quake3 2,730 (0.0% of core 0, 1 places).

Slow forms prof.py finds (% of core 0): 1.6%  [software-float] quake3:__adddf3  (6,827 calls of 159); 1.2%  [software-float] quake3:__muldf3  (8,194 calls of 100).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 1.14% of core 0's instructions and 7.24% of its write-cache entries, calls of about 2 KB or more 0.0% (quake3:memset 5,707 calls of 84 instructions; quake3:memcpy 5,471 calls of 57 instructions; quake3:memmove 2 calls of 25 instructions). The kernel's `__memcpy` 0.06% (byte paths 0.04%, 64-byte word loop 0.0%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): quake3:logf 3.36.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| quake3:Com_Init | 1 | 8,777,116 | 8,777,116 |

### halflife

Half-Life: hldemo1 played from second 31 to 43 of the level. `HL_MARK=31,43 halflife +map hldemo1`

Window: `hlmark: begins` to `hlmark: ends`: 12.1 s of the guest, 4,936 passes; core 0 39.0 M instructions (3.21 M a guest second, 7,892 a pass), all cores 286.6 M, 47 cores ran. Core 0 took 322,267 write-cache entries (290 distinct texels a pass that stored), and 20 of its passes ended full (1.7 a guest second); the workers' 1,121 (92.5 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 65.4 | 57.3 | 77.8 |
| libc copy/fill/str | 12.2 | 19.3 | 11.6 |
| libc other | 0.8 | 1.3 | 0.0 |
| libm | 0.3 | 0.1 | 0.1 |
| soft float, 64-bit | 9.3 | 3.0 | 1.4 |
| kernel | 5.6 | 15.8 | 0 |
| unknown | 6.4 | 3.3 | 9.1 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| halflife:memcpy | 8.6 | halflife:memcpy | 10.4 |
| halflife:PM_PlayerTraceExt | 6.8 | halflife:memset | 8.6 |
| halflife:CL_RunUsercmd | 5.4 | halflife:PM_PlayerTraceExt | 5.0 |
| halflife:PM_RecursiveHullCheck | 3.6 | kernel:handle_exception | 3.4 |
| halflife:__adddf3 | 3.1 | halflife:CL_ParsePacketEntities | 3.4 |
| halflife:memset | 2.6 | halflife:SE_EfxGive | 3.1 |
| halflife:se_ref_guard | 2.5 | halflife:PM_RecursiveHullCheck | 2.8 |
| halflife:PM_HullPointContents | 2.4 | halflife:CL_EmitEntities | 2.6 |

Core 0's passes ended full, where (passes): halflife:CL_ProcessPacket 8, halflife:memcpy 6, halflife:CL_ParsePacketEntities 4, ?0x88429000 1, halflife:R_SceneGive 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| halflife:R_StudioJob | 31.7 | halflife:memcpy | 37.2 |
| halflife:memcpy | 10.0 | halflife:R_StudioJob | 14.9 |
| ?0x883aa000 | 4.3 | ?0x883aa000 | 3.9 |
| halflife:R_WorldEntity | 3.1 | halflife:R_WorldMark | 3.4 |
| halflife:PM_RecursiveHullCheck | 2.5 | halflife:PM_RecursiveHullCheck | 2.7 |
| halflife:R_RecursiveLightPoint | 2.4 | halflife:R_StudioPointsOnWorker | 2.7 |

The workers' passes ended full, where (passes): halflife:memcpy 380, halflife:R_StudioPointsOnWorker 194, halflife:R_WorldMark 149, halflife:gpuDrawPut 60, ?0x883aa000 42.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): halflife 53,681 (0.14% of core 0, 9 places).

Slow forms prof.py finds (% of core 0): 4.9%  [independent-pixel-loop] halflife:CL_RunUsercmd+0xc8  (172,887 trips of 11); 1.9%  [software-float] halflife:__subdf3  (4,501 calls of 161); 1.6%  [software-float] halflife:__adddf3  (5,267 calls of 121); 1.5%  [software-float] halflife:__muldf3  (6,271 calls of 96); 1.2%  [software-float] halflife:__divdf3  (2,737 calls of 173).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 11.15% of core 0's instructions and 18.94% of its write-cache entries, calls of about 2 KB or more 1.88% (halflife:memcpy 27,388 calls of 110 instructions; halflife:memset 6,423 calls of 78 instructions; halflife:memset >=2KB 207 calls of 2,409 instructions). The kernel's `__memcpy` 0.1% (byte paths 0.01%, 64-byte word loop 0.0%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): halflife:S_PaintChannels 1.05, halflife:SE_CardChannel 1.04, halflife:CL_ComputePlayerOrigin 0.65, halflife:CL_RunUsercmd 0.49, halflife:Host_Frame 0.47, halflife:SE_CardTime 0.46.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): halflife:CL_WritePacket 149 0.97, halflife:CL_RunUsercmd 58 0.31.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| halflife:SV_ExecLoadLevel | 1 | 125,992,821 | 125,992,821 |
| halflife:SV_SpawnEntities | 1 | 73,030,129 | 73,030,129 |
| halflife:CL_ParseResourceList | 1 | 50,517,931 | 50,517,931 |
| halflife:CL_BatchResourceRequest | 1 | 43,165,526 | 43,165,526 |
| halflife:CL_Init | 1 | 41,236,870 | 41,236,870 |
| halflife:SV_SpawnServer.part.0 | 1 | 39,757,759 | 39,757,759 |

### classicube

ClassiCube: a new world, standing. `mkdir -p /tmp/classicube; echo singleplayerphysics=false >> /tmp/classicube/options.txt; CLASSICUBE_SEED=7 CLASSICUBE_STATS_MS=2000 classicube`

Window: `classicube: steady` to `the wall (50 s of the host)`: 75.5 s of the guest, 31,221 passes; core 0 157.5 M instructions (2.08 M a guest second, 5,044 a pass), all cores 895.4 M, 16 cores ran. Core 0 took 1,690,031 write-cache entries (103 distinct texels a pass that stored), and 1 of its passes ended full (0.0 a guest second); the workers' 59,534 (788.3 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 1.4 | 1.6 | 76.0 |
| libc copy/fill/str | 0.2 | 0.0 | 5.7 |
| libc other | 0.9 | 2.8 | 0.5 |
| soft float, 64-bit | 1.2 | 0.0 | 0.0 |
| kernel | 11.7 | 29.0 | 0 |
| window server | 0.0 | 0.1 | 0 |
| window client | 0.0 | 0.1 | 0 |
| unknown | 84.6 | 66.4 | 17.8 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x816ed000 | 14.5 | ?0x816ed000 | 13.3 |
| ?0x816bf000 | 11.9 | kernel:handle_exception | 8.1 |
| ?0x816fc000 | 8.4 | ?0x816fd000 | 7.4 |
| ?0x816d6000 | 8.0 | ?0x816a9000 | 6.4 |
| ?0x8168f000 | 4.5 | ?0x81699000 | 5.9 |
| ?0x816fd000 | 3.9 | ?0x816aa000 | 4.5 |
| ?0x8169a000 | 3.1 | ?0x816a7000 | 4.0 |
| ?0x81677000 | 2.7 | ?0x8169a000 | 4.0 |

Core 0's passes ended full, where (passes): nano-X:GrGetWindowInfo 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| classicube:mp3d_synth | 26.7 | classicube:memcpy | 19.7 |
| classicube:L3_decode | 19.4 | classicube:L3_decode | 16.3 |
| classicube:mp3d_synth_granule | 14.5 | classicube:mp3d_synth | 14.4 |
| classicube:L3_imdct36 | 7.9 | classicube:mp3d_synth_granule | 12.5 |
| classicube:memcpy | 3.9 | classicube:memset | 12.1 |
| classicube:L3_dct3_9 | 3.5 | classicube:L3_imdct36 | 6.2 |

The workers' passes ended full, where (passes): classicube:memcpy 18775, classicube:mp3d_synth_granule 10537, classicube:memset 10375, classicube:L3_decode 7531, ?0x816a3000 3015.

Slow forms prof.py finds (% of core 0): 1.2%  [software-float] classicube:__adddf3  (13,416 calls of 142); 1.1%  [independent-pixel-loop] ?0x816ed000  (161,004 trips of 11).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.16% of core 0's instructions and 0.01% of its write-cache entries, calls of about 2 KB or more 0.0% (classicube:memcpy 2,814 calls of 41 instructions; classicube:memset 185 calls of 8 instructions). The kernel's `__memcpy` 0.09% (byte paths 0.07%, 64-byte word loop 0.0%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): ?0x81699000 1.21.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| classicube:media_audio_open | 1 | 2,870,021 | 2,870,021 |
| classicube:m_mp3_open | 1 | 2,865,756 | 2,865,756 |
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| classicube:media_audio_half | 1 | 214,361 | 214,361 |

### tdawn-menu

Tiberian Dawn: its start and menu. `tdawn FROMINSTALL`

Window: 0 s after the command to 40 s later: 40.3 s of the guest, 16,446 passes; core 0 134.0 M instructions (3.32 M a guest second, 8,145 a pass), all cores 134.0 M, 1 cores ran. Core 0 took 372,211 write-cache entries (40 distinct texels a pass that stored), and 46 of its passes ended full (1.1 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 82.7 | 18.6 | 0 |
| libc copy/fill/str | 3.3 | 4.6 | 0 |
| libc other | 2.6 | 10.5 | 0 |
| kernel | 11.1 | 65.3 | 0 |
| window server | 0.2 | 0.8 | 0 |
| window client | 0.0 | 0.2 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| tdawn:host_event | 34.0 | tdawn:_ZN11VectorClassI9CellClassE6ResizeEjPKS0_ | 8.8 |
| tdawn:_ZN24WWKeyboardClassShaderEmu23Fill_Buffer_From_SystemEv | 21.6 | kernel:timekeeping_advance | 5.6 |
| tdawn:host_ms | 15.9 | kernel:handle_exception | 5.3 |
| tdawn:seglPalette | 5.3 | kernel:timekeeping_update | 5.1 |
| tdawn:_ZN15WWKeyboardClass3GetEv | 3.1 | tdawn:host_ms | 3.7 |
| tdawn:memset | 1.1 | tdawn:__libc_malloc_impl | 2.8 |
| kernel:free_pcppages_bulk | 0.8 | kernel:alloc_pages_run | 2.2 |
| kernel:timekeeping_advance | 0.8 | tdawn:memcpy | 2.2 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 25, tdawn:_Z41__static_initialization_and_destruction_0v.lto_priv.4 3, tdawn:_Z41__static_initialization_and_destruction_0v.lto_priv.3 3, kernel:call_rcu 2, tdawn:memcpy 2.

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 1.42% of core 0's instructions and 3.29% of its write-cache entries, calls of about 2 KB or more 0.2% (tdawn:memset 22,005 calls of 57 instructions; tdawn:memcpy 12,135 calls of 39 instructions; tdawn:memset >=2KB 17 calls of 15,091 instructions). The kernel's `__memcpy` 0.12% (byte paths 0.09%, 64-byte word loop 0.01%).

### tdawn

Tiberian Dawn: mission 3 played by itself, as fast as it goes. `TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_FRAMES=1300 tdawn FROMINSTALL`

Window: `the command` to `tdawn: done`: 44.9 s of the guest, 18,909 passes; core 0 147.0 M instructions (3.28 M a guest second, 7,775 a pass), all cores 268.4 M, 43 cores ran. Core 0 took 1,443,967 write-cache entries (151 distinct texels a pass that stored), and 742 of its passes ended full (16.5 a guest second); the workers' 1,252 (27.9 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 72.8 | 54.0 | 89.7 |
| libc copy/fill/str | 8.8 | 16.6 | 2.8 |
| libc other | 4.8 | 5.7 | 6.9 |
| kernel | 11.4 | 19.9 | 0 |
| window server | 0.2 | 0.2 | 0 |
| unknown | 2.0 | 3.5 | 0.5 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| tdawn:_ZN13InfantryClass2AIEv | 10.3 | tdawn:memset | 7.9 |
| tdawn:_ZN10LogicClass2AIEv | 9.8 | tdawn:memcpy | 7.8 |
| tdawn:_ZNK11TechnoClass15Greatest_ThreatE10ThreatType | 6.9 | tdawn:_ZN8MapClass10Init_CellsEv | 6.8 |
| tdawn:_ZN12DisplayClass7Draw_ItEb | 4.9 | tdawn:_ZN12DisplayClass7Draw_ItEb | 5.4 |
| tdawn:_ZN9FootClass14ShaderEmu_WalkEssP8PathType10FacingTypeS2_ii8MoveType | 3.1 | tdawn:_ZN9FootClass4MarkE8MarkType | 3.5 |
| tdawn:memset | 2.9 | tdawn:_ZN13InfantryClass2AIEv | 3.5 |
| tdawn:_ZN9FootClass4MarkE8MarkType | 2.8 | tdawn:_ZNK11TechnoClass15Greatest_ThreatE10ThreatType | 3.1 |
| tdawn:memcpy | 2.7 | tdawn:_ZN10LogicClass2AIEv | 2.8 |

Core 0's passes ended full, where (passes): tdawn:memset 324, tdawn:_ZN8MapClass10Init_CellsEv 320, kernel:alloc_pages_run 46, tdawn:memcpy 13, tdawn:_ZL3putP5ShapePKhPK10host_frame.lto_priv.0 13.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| tdawn:frames_job | 40.0 | tdawn:seglSprites | 23.8 |
| tdawn:ShaderEmu_Frame_Step | 10.8 | tdawn:ShaderEmu_Frame_Step | 12.4 |
| tdawn:_ZN9FootClass14ShaderEmu_WalkEssP8PathType10FacingTypeS2_ii8MoveType | 7.2 | tdawn:scene_job | 12.2 |
| tdawn:memcpy_any | 6.9 | tdawn:memcpy_any | 11.1 |
| tdawn:seglSprites | 6.0 | tdawn:memcpy | 6.4 |
| tdawn:scene_job | 5.3 | tdawn:frames_job | 4.9 |

The workers' passes ended full, where (passes): tdawn:seglSprites 848, tdawn:scene_job 329, tdawn:memcpy_any 18, tdawn:matrix_block_write.part.0 18, ?0x81b88000 15.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 1,749 (0.0% of core 0, 164 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 5.87% of core 0's instructions and 16.09% of its write-cache entries, calls of about 2 KB or more 0.99% (tdawn:memcpy 40,708 calls of 105 instructions; tdawn:memset 29,159 calls of 72 instructions; tdawn:memset >=2KB 42 calls of 32,857 instructions). The kernel's `__memcpy` 0.13% (byte paths 0.09%, 64-byte word loop 0.01%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): tdawn:_ZN8MapClass10Init_CellsEv 9 0.5, tdawn:_ZN14FixedHeapClass8Set_HeapEiPv 16 0.15, tdawn:_ZN12DisplayClass7Draw_ItEb 10 0.14, tdawn:host_scene_open 2 0.11.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| tdawn:_Z11Select_Gameb | 1 | 17,705,809 | 17,705,809 |
| tdawn:_Z14Start_ScenarioPcb | 1 | 13,102,545 | 13,102,545 |
| tdawn:_Z13Read_ScenarioPc | 1 | 12,569,559 | 12,569,559 |
| tdawn:_Z17Read_Scenario_IniPcb | 1 | 12,182,717 | 12,182,717 |
| tdawn:_Z9Init_GameiPPc | 1 | 10,057,626 | 10,057,626 |
| tdawn:_ZN12DisplayClass8Read_INIER10CCINIClass | 1 | 5,722,796 | 5,722,796 |

### ralert

Red Alert: its film, then the Soviet mission played by itself. `TDAWN_AUTO=1 RALERT_SIDE=soviet RALERT_SEED=7 RALERT_FRAMES=1300 ralert`

Window: `ralert: begun at frame 0` to `ralert: done`: 25.1 s of the guest, 10,335 passes; core 0 82.4 M instructions (3.29 M a guest second, 7,972 a pass), all cores 190.6 M, 11 cores ran. Core 0 took 522,383 write-cache entries (112 distinct texels a pass that stored), and 8 of its passes ended full (0.3 a guest second); the workers' 213 (8.5 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 88.0 | 62.5 | 90.6 |
| libc copy/fill/str | 3.9 | 9.2 | 2.6 |
| libc other | 0.7 | 1.6 | 6.4 |
| kernel | 5.0 | 18.3 | 0 |
| window server | 0.0 | 0.1 | 0 |
| unknown | 2.4 | 8.3 | 0.3 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ralert:_ZN13BuildingClass2AIEv | 13.8 | ralert:_ZN10RadarClass7Draw_ItEb | 7.4 |
| ralert:_ZN10LogicClass2AIEv | 12.8 | ralert:memcpy | 5.9 |
| ralert:_ZN13InfantryClass2AIEv | 7.6 | ralert:_Z9Main_GameiPPc | 4.5 |
| ralert:_ZNK11TechnoClass15Greatest_ThreatE10ThreatType | 6.8 | ?0x81d89000 | 4.0 |
| ralert:_ZN10RadarClass7Draw_ItEb | 5.7 | ralert:_ZN10DriveClass12While_MovingEv | 4.0 |
| ralert:_ZN11TechnoClass2AIEv | 4.5 | ralert:memset | 3.2 |
| ralert:_ZN9UnitClass2AIEv | 2.6 | kernel:handle_exception | 2.9 |
| ralert:_ZN11TechnoClass4LookEb | 2.5 | ralert:_ZN10LogicClass2AIEv | 2.8 |

Core 0's passes ended full, where (passes): ralert:memcpy 4, ralert:memset 3, ralert:_Z11Build_FramePKvtPv 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ralert:frames_job | 51.1 | ralert:ShaderEmu_Frame_Step | 19.6 |
| ralert:ShaderEmu_Frame_Step | 16.0 | ralert:memcpy | 19.2 |
| ralert:seglSprites | 9.7 | ralert:memcpy_any | 16.9 |
| ralert:scene_job | 7.4 | ralert:scene_job | 14.0 |
| ralert:memcpy_any | 6.4 | ralert:seglSprites | 10.6 |
| ralert:_ZL18ShaderEmu_Sort_Jobjj.lto_priv.0 | 1.8 | ralert:frames_job | 6.5 |

The workers' passes ended full, where (passes): ralert:seglSprites 120, ralert:scene_job 40, ralert:memcpy_any 20, ralert:memset 12, ralert:memcpy 8.

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.96% of core 0's instructions and 9.2% of its write-cache entries, calls of about 2 KB or more 0.01% (ralert:memcpy 22,759 calls of 74 instructions; ralert:memset 9,764 calls of 94 instructions; ralert:memmove 189 calls of 103 instructions). The kernel's `__memcpy` 0.15% (byte paths 0.14%, 64-byte word loop 0.0%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| ralert:_Z9Init_GameiPPc | 1 | 65,517,994 | 65,517,994 |
| ralert:_Z11Select_Gameb | 1 | 46,194,136 | 46,194,136 |
| ralert:_Z17Read_Scenario_INIPcb | 1 | 30,506,563 | 30,506,563 |
| ralert:_ZN12DisplayClass8Read_INIER10CCINIClass | 1 | 17,815,304 | 17,815,304 |
| ralert:_Z10Play_MoviePKc9ThemeTypebb | 3 | 10,886,024 | 32,582,578 |
| ralert:_ZN12GScreenClass4InitE11TheaterType | 1 | 10,662,572 | 10,662,572 |

### nes

NES: thwaite. `NES_FRAMES=1500 nes thwaite`

Window: `the command` to `nes: done`: 8.2 s of the guest, 3,379 passes; core 0 27.3 M instructions (3.32 M a guest second, 8,082 a pass), all cores 27.3 M, 1 cores ran. Core 0 took 230,779 write-cache entries (121 distinct texels a pass that stored), and 38 of its passes ended full (4.6 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 55.5 | 54.8 | 0 |
| libc copy/fill/str | 2.5 | 5.5 | 0 |
| libc other | 0.6 | 1.2 | 0 |
| soft float, 64-bit | 0.2 | 0.0 | 0 |
| kernel | 9.5 | 21.5 | 0 |
| window server | 1.5 | 1.5 | 0 |
| window client | 0.0 | 0.1 | 0 |
| unknown | 30.2 | 15.4 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nes:ppu_readhigh | 11.7 | nes:nes_renderframe.constprop.0 | 8.8 |
| ?0x81659000 | 9.0 | nes:nes6502_execute | 6.8 |
| nes:nes_renderframe.constprop.0 | 8.5 | nes:idle_loop | 4.8 |
| ?0x81487000 | 8.2 | ?0x81653000 | 4.8 |
| nes:mcwx_longjmp | 7.0 | ?0x81659000 | 4.4 |
| nes:rc_pad | 3.9 | nes:se_apu_quarter | 3.9 |
| nes:se_apu_quarter | 3.0 | nes:memset | 3.5 |
| ?0x8164f000 | 2.8 | nes:ppu_lines_settle | 3.1 |

Core 0's passes ended full, where (passes): nes:memset 24, kernel:alloc_pages_run 3, nes:fps_caption 3, kernel:rmqueue_bulk 2, kernel:__memset 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 6,470 (0.02% of core 0, 159 places).

Slow forms prof.py finds (% of core 0): 2.4%  [division-in-loop] nes:nes_renderframe.constprop.0+0xe50  (21,896 trips of 29); 2.2%  [division-in-loop] nes:nes_renderframe.constprop.0+0xe38  (22,006 trips of 27); 2.0%  [division-in-loop] nes:se_apu_quarter+0x78  (18,000 trips of 31).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.07% of core 0's instructions and 5.48% of its write-cache entries, calls of about 2 KB or more 0.49% (nes:memcpy 3,765 calls of 96 instructions; nes:memset >=2KB 12 calls of 11,018 instructions; nes:memset 3,031 calls of 16 instructions). The kernel's `__memcpy` 0.26% (byte paths 0.2%, 64-byte word loop 0.02%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): ?0x81612000 11 0.48.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nes:ppu_draw | 1 | 456,331 | 456,331 |
| nano-X:index_cell | 2 | 53,832 | 86,110 |

### nxphys

nxphys: the wall, 600 substeps. `PHYS_SCENE=1 PHYS_STEPS=600 nxphys`

Window: `the command` to `physstat: done`: 4.5 s of the guest, 1,823 passes; core 0 5.1 M instructions (1.13 M a guest second, 2,805 a pass), all cores 635.0 M, 256 cores ran. Core 0 took 62,435 write-cache entries (56 distinct texels a pass that stored), and 59 of its passes ended full (13.0 a guest second); the workers' 669 (147.4 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 23.0 | 19.8 | 97.3 |
| libc copy/fill/str | 0.2 | 0.0 | 0 |
| libc other | 0.2 | 0.3 | 0 |
| kernel | 41.5 | 65.2 | 0 |
| window server | 6.5 | 6.0 | 0 |
| unknown | 28.6 | 8.7 | 2.7 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x81490000 | 13.0 | kernel:handle_exception | 12.0 |
| ?0x8148f000 | 12.5 | kernel:alloc_pages_run | 10.0 |
| kernel:alloc_pages_run | 12.0 | nxphys:main | 7.1 |
| nxphys:main | 7.7 | kernel:timekeeping_advance | 3.7 |
| kernel:handle_exception | 4.2 | kernel:timekeeping_update | 3.4 |
| nxphys:look_at_jobs | 3.4 | ?0x8149e000 | 3.2 |
| nxphys:ask_picture | 2.4 | nxphys:seat | 2.5 |
| kernel:timekeeping_advance | 2.3 | kernel:__fget_light | 2.2 |

Core 0's passes ended full, where (passes): ?0x8148f000 28, kernel:alloc_pages_run 23, ?0x8149e000 4, kernel:do_mmap 1, kernel:kernel_waitid 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxphys:phys_step | 45.1 | nxphys:phys_step | 70.1 |
| nxphys:share_advance | 19.9 | nxphys:box_box | 10.4 |
| nxphys:woken | 9.6 | nxphys:look | 5.4 |
| nxphys:box_box | 8.7 | nxphys:share_advance | 4.7 |
| nxphys:look | 7.0 | nxphys:share_work | 3.4 |
| nxphys:phys_grid | 2.3 | nxphys:phys_quads | 2.9 |

The workers' passes ended full, where (passes): nxphys:phys_step 638, nxphys:box_box 15, nxphys:poses_made 8, nxphys:share_work 5, nxphys:look 2.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 4,918 (0.1% of core 0, 167 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.11% of core 0's instructions and 0.44% of its write-cache entries, calls of about 2 KB or more 0.0% (nano-X:memcpy 56 calls of 113 instructions; nano-X:memset 9 calls of 86 instructions; nxphys:memcpy 16 calls of 33 instructions). The kernel's `__memcpy` 0.52% (byte paths 0.26%, 64-byte word loop 0.15%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nxphys:share_work | 1 | 564,792 | 564,792 |
| nxphys:seglInit | 1 | 103,186 | 103,186 |
| nxphys:take_workers | 1 | 90,474 | 90,474 |

### pinball

Pinball: its demo, 1,800 frames. `PINBALL_DEMO=1800 nxpinball`

Window: `the command` to `pinball: 1800 frames`: 16.8 s of the guest, 6,889 passes; core 0 55.9 M instructions (3.32 M a guest second, 8,115 a pass), all cores 55.9 M, 1 cores ran. Core 0 took 492,554 write-cache entries (108 distinct texels a pass that stored), and 17 of its passes ended full (1.0 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 20.3 | 4.8 | 0 |
| libc copy/fill/str | 0.1 | 0.2 | 0 |
| libc other | 0.4 | 0.8 | 0 |
| libm | 1.4 | 1.2 | 0 |
| soft float, 64-bit | 18.5 | 1.8 | 0 |
| kernel | 14.4 | 44.8 | 0 |
| window server | 42.6 | 41.2 | 0 |
| window client | 2.0 | 5.3 | 0 |
| unknown | 0.3 | 0.0 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nxpinball:tick | 15.7 | nano-X:gpu_drawhorzline | 21.0 |
| nano-X:gpu_drawhorzline | 11.1 | kernel:__memcpy | 6.1 |
| nxpinball:__muldf3 | 9.5 | kernel:handle_exception | 4.6 |
| nxpinball:__adddf3 | 7.9 | nano-X:gpu_frameblit | 2.8 |
| nano-X:drawrow | 6.9 | kernel:__memset | 2.7 |
| nano-X:GdClipPoint | 3.1 | kernel:timekeeping_advance | 1.8 |
| nano-X:drawarcsegment | 2.7 | nxpinball:tick | 1.8 |
| nano-X:GdSetDash | 1.8 | kernel:timekeeping_update | 1.7 |

Core 0's passes ended full, where (passes): kernel:__memcpy 8, kernel:alloc_pages_run 2, kernel:__wake_up_common_lock 1, kernel:kernel_waitid 1, kernel:sock_read_iter 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nxpinball 1,814 (0.0% of core 0, 17 places), nano-X 686,192 (1.23% of core 0, 148 places).

Slow forms prof.py finds (% of core 0): 10.7%  [division-in-loop] nxpinball:tick+0x268  (664,909 trips of 9); 9.5%  [software-float] nxpinball:__muldf3  (52,932 calls of 100); 5.0%  [software-float] nxpinball:__adddf3  (17,698 calls of 158); 3.6%  [software-float] nxpinball:__subdf3  (12,739 calls of 157); 1.3%  [software-float] nano-X:__adddf3  (8,028 calls of 94).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.37% of core 0's instructions and 0.67% of its write-cache entries, calls of about 2 KB or more 0.0% (nano-X:memcpy 3,950 calls of 35 instructions; nxpinball:memcpy 912 calls of 28 instructions; nxpinball:memset 183 calls of 81 instructions). The kernel's `__memcpy` 0.77% (byte paths 0.01%, 64-byte word loop 0.54%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): nxpinball:__sindf 8.97, nxpinball:__cosdf 8.61, nano-X:GdFillPoly 1.93, nxpinball:sincosf 0.83, nano-X:wrapper_cmp 0.22, nxpinball:__rem_pio2f 0.1.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxpinball:art_make | 1 | 6,188,205 | 6,188,205 |
| nxpinball:art_base | 1 | 3,337,916 | 3,337,916 |
| nxpinball:GrReadArea | 2 | 1,380,295 | 2,760,254 |
| nxpinball:GrGetFontInfo | 3 | 1,071,292 | 3,200,783 |
| nxpinball:nx_more | 7 | 842,224 | 3,148,060 |
| nxpinball:recv | 7 | 842,164 | 3,147,997 |

### micropolis

Micropolis: Haight, 1,000 ticks simulated. `MICROPOLIS_SUM=/usr/share/micropolis/cities/haight.cty,1000 nxmicropolis`

Window: `the command` to `micropolis: `: 86.6 s of the guest, 35,180 passes; core 0 288.0 M instructions (3.32 M a guest second, 8,186 a pass), all cores 288.0 M, 1 cores ran. Core 0 took 1,672,496 write-cache entries (56 distinct texels a pass that stored), and 31 of its passes ended full (0.4 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 95.5 | 82.1 | 0 |
| libc copy/fill/str | 0.0 | 0.7 | 0 |
| libc other | 0.0 | 0.1 | 0 |
| kernel | 4.4 | 17.1 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nxmicropolis:_ZN10Micropolis12animateTilesEv | 39.4 | nxmicropolis:_ZN10Micropolis12animateTilesEv | 58.3 |
| nxmicropolis:_ZN10Micropolis7mapScanEii | 7.7 | nxmicropolis:_ZN8PositionaSERKS_ | 3.3 |
| nxmicropolis:_ZN10Micropolis17testForConductiveERK8Position10Direction2 | 7.1 | nxmicropolis:_ZN10Micropolis11doPowerScanEv | 3.0 |
| nxmicropolis:_ZN10Micropolis5tryGoERK8Position10Direction2 | 6.4 | kernel:timekeeping_advance | 2.7 |
| nxmicropolis:_ZN10Micropolis12setZonePowerERK8Position | 4.8 | kernel:timekeeping_update | 2.4 |
| nxmicropolis:_ZN8Position4moveE10Direction2 | 4.0 | nxmicropolis:_ZN10Micropolis5tryGoERK8Position10Direction2 | 2.2 |
| nxmicropolis:_ZN10Micropolis6doRoadERK8Position | 3.2 | kernel:handle_exception | 2.0 |
| nxmicropolis:_ZN10Micropolis11doPowerScanEv | 3.1 | nxmicropolis:_ZN10Micropolis6doRoadERK8Position | 1.9 |

Core 0's passes ended full, where (passes): nxmicropolis:memset 28, kernel:__memcpy 1, kernel:do_iter_readv_writev 1, kernel:alloc_pages_run 1.

Slow forms prof.py finds (% of core 0): 39.4%  [independent-pixel-loop] nxmicropolis:_ZN10Micropolis12animateTilesEv+0x20  (12,000,000 trips of 9); 1.3%  [independent-pixel-loop] nxmicropolis:_ZN10Micropolis6doRoadERK8Position+0xb0  (154,010 trips of 25); 1.2%  [independent-pixel-loop] nxmicropolis:_ZN8Position4moveE10Direction2+0xe0  (172,455 trips of 20).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.04% of core 0's instructions and 0.73% of its write-cache entries, calls of about 2 KB or more 0.03% (nxmicropolis:memset >=2KB 18 calls of 4,005 instructions; nxmicropolis:memset 189 calls of 221 instructions; nxmicropolis:memcpy 121 calls of 133 instructions). The kernel's `__memcpy` 0.02% (byte paths 0.01%, 64-byte word loop 0.0%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxmicropolis:mp_sum_test | 1 | 275,217,758 | 275,217,758 |
| nxmicropolis:mp_load | 1 | 13,487,162 | 13,487,162 |
| nxmicropolis:_ZN10Micropolis8loadCityERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE | 1 | 13,486,390 | 13,486,390 |
| nxmicropolis:_ZN10Micropolis8loadFileERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE.part.0 | 1 | 6,776,785 | 6,776,785 |
| nxmicropolis:_ZN10Micropolis9doSimInitEv | 2 | 6,677,867 | 6,757,517 |
| nxmicropolis:_ZN10Micropolis11doPowerScanEv | 16 | 2,616,060 | 2,989,516 |

### openttd

OpenTTD: a busy game loaded, scrolled, zoomed, its windows. `OPENTTD_PLAY=1 OPENTTD_PAUSE=1 openttd -g /usr/share/openttd/busy.sav`

Window: `play PL-GO` to `play PL-DONE`: 55.1 s of the guest, 23,293 passes; core 0 150.1 M instructions (2.73 M a guest second, 6,443 a pass), all cores 721.6 M, 32 cores ran. Core 0 took 2,737,701 write-cache entries (163 distinct texels a pass that stored), and 4,255 of its passes ended full (77.3 a guest second); the workers' 1,133 (20.6 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 65.8 | 38.8 | 95.1 |
| libc copy/fill/str | 3.7 | 7.8 | 2.5 |
| libc other | 19.7 | 43.0 | 1.9 |
| soft float, 64-bit | 0.9 | 0.0 | 0.5 |
| kernel | 8.6 | 9.8 | 0 |
| unknown | 1.2 | 0.5 | 0.1 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| openttd:memcpy_any | 19.0 | openttd:memcpy_any | 42.2 |
| openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh | 16.3 | openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh | 20.9 |
| openttd:_ZN21Blitter_8bppOptimized6EncodeE10SpriteTypeRK13SpriteCollMapIN12SpriteLoader6SpriteEER15SpriteAllocator | 14.8 | openttd:memset | 5.7 |
| openttd:_ZL15ResizeSpriteOutR13SpriteCollMapIN12SpriteLoader6SpriteEE9ZoomLevel | 5.3 | openttd:_ZL15ResizeSpriteOutR13SpriteCollMapIN12SpriteLoader6SpriteEE9ZoomLevel | 5.0 |
| openttd:_ZN16RandomAccessFile8ReadByteEv | 3.8 | openttd:_ZN21Blitter_8bppOptimized6EncodeE10SpriteTypeRK13SpriteCollMapIN12SpriteLoader6SpriteEER15SpriteAllocator | 2.5 |
| openttd:_ZL20ViewportAddLandscapev | 3.6 | openttd:memcpy | 2.1 |
| openttd:_ZN21Blitter_8bppOptimized4DrawEPN7Blitter13BlitterParamsE11BlitterMode9ZoomLevel | 3.1 | kernel:handle_exception | 1.4 |
| openttd:memset | 2.8 | openttd:_ZN21Blitter_8bppOptimized4DrawEPN7Blitter13BlitterParamsE11BlitterMode9ZoomLevel | 1.2 |

Core 0's passes ended full, where (passes): openttd:memcpy_any 3327, openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh 569, kernel:__memcpy 102, openttd:memset 93, openttd:_ZL15ResizeSpriteOutR13SpriteCollMapIN12SpriteLoader6SpriteEE9ZoomLevel 49.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| openttd:_ZL20ViewportAddLandscapev | 20.6 | openttd:memset | 17.4 |
| openttd:_Z23AddSortableSpriteToDrawjjiiiRK12SpriteBoundsbPK9SubSprite | 12.3 | openttd:_ZL19AddTileSpriteToDrawjjiiiPK9SubSpriteii | 15.7 |
| openttd:_ZN21Blitter_8bppOptimized4DrawEPN7Blitter13BlitterParamsE11BlitterMode9ZoomLevel | 10.4 | openttd:_ZN21Blitter_8bppOptimized4DrawEPN7Blitter13BlitterParamsE11BlitterMode9ZoomLevel | 11.2 |
| openttd:_Z12GetRawSpritej10SpriteTypeP15SpriteAllocatorP13SpriteEncoder | 6.8 | openttd:_ZL14VpScreenExtentP18ParentSpriteToDrawi | 7.3 |
| openttd:_Z10GfxBlitterILi4ELb0EEvPK6Spriteii11BlitterModePK9SubSpritej9ZoomLevelPK13DrawPixelInfo.constprop.0 | 6.0 | openttd:_Z23AddSortableSpriteToDrawjjiiiRK12SpriteBoundsbPK9SubSprite | 6.4 |
| openttd:_ZL14DrawTile_TreesP8TileInfo | 5.4 | openttd:_ZL20ViewportAddLandscapev | 6.3 |

The workers' passes ended full, where (passes): openttd:memcpy_any 390, openttd:_ZL29ViewportSortParentSpritesKeptPSt6vectorIP18ParentSpriteToDraw11VpAllocatorIS1_EE 377, openttd:_ZSt16__introsort_loopIN9__gnu_cxx17__normal_iteratorIPSt4pairIxP18ParentSpriteToDrawESt6vectorIS5_11VpAllocatorIS5_EEEEiNS0_5__ops15_Iter_comp_iterIZL29ViewportSortParentSpritesKeptPS7_IS4_S8_IS4_EEEUlRKT_RKT0_E_EEEvSH_SH_SK_T1_ 179, openttd:memcpy 101, openttd:_ZL20ViewportAddLandscapev 42.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): openttd 9,453 (0.01% of core 0, 21 places).

Slow forms prof.py finds (% of core 0): 7.7%  [byte-copy] openttd:_ZN21Blitter_8bppOptimized6EncodeE10SpriteTypeRK13SpriteCollMapIN12SpriteLoader6SpriteEER15SpriteAllocator+0x1d0  (1,676,961 trips of 7); 7.1%  [independent-pixel-loop] openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh+0xeb0  (855,657 trips of 12); 6.4%  [independent-pixel-loop] openttd:memcpy_any+0x454  (416,536 trips of 23); 6.3%  [independent-pixel-loop] openttd:memcpy_any+0x164  (409,880 trips of 23); 5.4%  [independent-pixel-loop] openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh+0xea4  (855,657 trips of 9).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.63% of core 0's instructions and 7.84% of its write-cache entries, calls of about 2 KB or more 1.78% (openttd:memmove 38,739 calls of 767 instructions; openttd:memset >=2KB 609 calls of 4,386 instructions; openttd:memset 18,279 calls of 80 instructions). The kernel's `__memcpy` 0.28% (byte paths 0.0%, 64-byte word loop 0.19%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): openttd:pow 0.58, openttd:_Z13ViewportFlushv 0.19.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): openttd:_ZN21Blitter_8bppOptimized6EncodeE10SpriteTypeRK13SpriteCollMapIN12SpriteLoader6SpriteEER15SpriteAllocator 609 1.78.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| openttd:_Z12SwitchToMode10SwitchMode | 1 | 185,920,586 | 185,920,586 |
| openttd:_Z8SafeLoadRKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE17SaveLoadOperation16DetailedFileType8GameMode12SubdirectorySt10shared_ptrI10LoadFilterE | 1 | 185,762,692 | 185,762,692 |
| openttd:_Z10SaveOrLoadSt17basic_string_viewIcSt11char_traitsIcEE17SaveLoadOperation16DetailedFileType12Subdirectoryb | 2 | 117,885,877 | 185,762,661 |
| openttd:_ZL6DoLoadSt10shared_ptrI10LoadFilterEb | 2 | 117,880,960 | 185,760,950 |
| openttd:_ZL13LoadIntroGameb | 1 | 63,536,602 | 63,536,602 |
| openttd:_ZN11MusicSystem4PlayEv | 1 | 53,804,091 | 53,804,091 |

### openttd-title

OpenTTD: its title screen (a game behind the menu). `openttd`

Window: 40 s after the command to 20 s later: 20.3 s of the guest, 8,592 passes; core 0 67.2 M instructions (3.31 M a guest second, 7,821 a pass), all cores 67.2 M, 1 cores ran. Core 0 took 619,635 write-cache entries (122 distinct texels a pass that stored), and 401 of its passes ended full (19.7 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 77.7 | 58.9 | 0 |
| libc copy/fill/str | 3.4 | 5.9 | 0 |
| libc other | 8.3 | 11.1 | 0 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 10.6 | 24.1 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| openttd:_ZN20MissingGlyphSearcher17FindMissingGlyphsEv | 8.9 | openttd:_ZL14ResizeSpriteInR13SpriteCollMapIN12SpriteLoader6SpriteEE9ZoomLevelS4_ | 10.5 |
| openttd:_Z19InitializeLandscapev | 5.6 | openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh | 7.4 |
| openttd:_ZN15SpriteFontCache14MapCharToGlyphEDib | 5.4 | openttd:lzma_decode | 6.1 |
| openttd:lzma_decode | 4.6 | openttd:_Z19AllocateSpriteCachej | 6.1 |
| openttd:_ZL14ResizeSpriteInR13SpriteCollMapIN12SpriteLoader6SpriteEE9ZoomLevelS4_ | 4.3 | openttd:_Z19InitializeLandscapev | 5.3 |
| openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh | 3.7 | kernel:__memcpy | 4.9 |
| openttd:_Z10DecodeUtf8St17basic_string_viewIcSt11char_traitsIcEE | 3.3 | openttd:__libc_malloc_impl | 4.7 |
| openttd:__libc_malloc_impl | 3.1 | openttd:_Z14LoadNextSpritejR10SpriteFilej | 3.2 |

Core 0's passes ended full, where (passes): openttd:_Z19AllocateSpriteCachej 115, openttd:_ZL14ResizeSpriteInR13SpriteCollMapIN12SpriteLoader6SpriteEE9ZoomLevelS4_ 102, kernel:__memcpy 70, openttd:_Z18DecodeSingleSpritePN12SpriteLoader6SpriteER10SpriteFilej10SpriteTypexh9ZoomLevel10EnumBitSetI15SpriteComponenthLS7_3EEh 29, openttd:memcpy 11.

Slow forms prof.py finds (% of core 0): 3.9%  [division-in-loop] openttd:_ZL14ResizeSpriteInR13SpriteCollMapIN12SpriteLoader6SpriteEE9ZoomLevelS4_+0x194  (144,672 trips of 18); 2.5%  [division-in-loop] openttd:_ZN15SpriteFontCache14MapCharToGlyphEDib+0x54  (139,089 trips of 12); 1.4%  [polling-wait] openttd:_ZN15SpriteFontCache14MapCharToGlyphEDib+0x40  (139,075 trips of 7); 1.4%  [division-in-loop] openttd:_ZN15SpriteFontCache14MapCharToGlyphEDib+0x5c  (139,075 trips of 7).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 1.78% of core 0's instructions and 5.55% of its write-cache entries, calls of about 2 KB or more 0.1% (openttd:memcpy 16,509 calls of 42 instructions; openttd:memset 4,895 calls of 106 instructions; openttd:memmove 3,182 calls of 105 instructions). The kernel's `__memcpy` 0.73% (byte paths 0.04%, 64-byte word loop 0.55%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| openttd:_ZL13LoadIntroGameb | 2 | 47,232,510 | 49,613,192 |
| openttd:_ZN21VideoDriver_ShaderEmu8MainLoopEv | 1 | 46,534,210 | 46,534,210 |
| openttd:_ZN11VideoDriver4TickEv | 1 | 46,533,881 | 46,533,881 |
| openttd:_ZN11VideoDriver8GameLoopEv | 1 | 46,533,826 | 46,533,826 |
| openttd:_Z8GameLoopv | 1 | 46,532,125 | 46,532,125 |
| openttd:_Z10SaveOrLoadSt17basic_string_viewIcSt11char_traitsIcEE17SaveLoadOperation16DetailedFileType12Subdirectoryb | 2 | 45,280,816 | 47,705,375 |

### video-mpg

nxvideo: Big Buck Bunny, MPEG-1 320 x 176 at 24 pictures a second. `NXVIDEO_ONCE=1 nxvideo`

Window: 3 s after the command to 20 s later: 20.3 s of the guest, 9,935 passes; core 0 55.8 M instructions (2.74 M a guest second, 5,618 a pass), all cores 677.3 M, 25 cores ran. Core 0 took 514,770 write-cache entries (65 distinct texels a pass that stored), and 38 of its passes ended full (1.9 a guest second); the workers' 20,142 (990.5 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 18.1 | 14.3 | 88.1 |
| libc other | 0.0 | 0.1 | 0 |
| kernel | 24.0 | 46.0 | 0 |
| window server | 5.8 | 2.3 | 0 |
| unknown | 52.0 | 37.4 | 11.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x819d6000 | 33.6 | ?0x819c4000 | 31.9 |
| kernel:handle_exception | 7.2 | kernel:handle_exception | 18.2 |
| ?0x819ce000 | 6.0 | nxvideo:dispatch | 3.6 |
| nxvideo:dispatch | 5.8 | kernel:ktime_get_ts64 | 3.3 |
| kernel:ktime_get_ts64 | 3.9 | kernel:__memcpy | 2.3 |
| nxvideo:mpeg_ready | 3.4 | nxvideo:main | 2.2 |
| ?0x819c4000 | 2.9 | kernel:timekeeping_advance | 2.1 |
| ?0x819d7000 | 2.7 | kernel:timekeeping_update | 1.9 |

Core 0's passes ended full, where (passes): ?0x819c4000 24, kernel:handle_exception 7, nxvideo:main 2, nxvideo:mpeg_ready 2, nxvideo:m_now_ms 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxvideo:plm_video_decode_block | 26.8 | ?0x819c4000 | 41.0 |
| nxvideo:plm_audio_decode_frame | 24.7 | nxvideo:plm_video_decode_block | 13.3 |
| nxvideo:plm_video_idct | 13.3 | nxvideo:plm_audio_decode_frame | 13.1 |
| ?0x819c4000 | 8.7 | nxvideo:plm_audio_idct36 | 8.6 |
| nxvideo:plm_video_process_macroblock.constprop.0 | 5.0 | nxvideo:plm_video_process_macroblock.constprop.0 | 7.5 |
| nxvideo:plm_video_process_macroblock.constprop.1 | 4.0 | nxvideo:plm_video_idct | 5.8 |

The workers' passes ended full, where (passes): ?0x819c4000 16951, nxvideo:plm_video_process_macroblock.constprop.0 2647, nxvideo:decode_chunk 224, nxvideo:plm_video_idct 106, nxvideo:decode_gop 48.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 102,270 (0.18% of core 0, 53 places).

Slow forms prof.py finds (% of core 0): 3.9%  [independent-pixel-loop] nxvideo:dispatch+0x64  (146,472 trips of 15).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.0% of core 0's instructions and 0.01% of its write-cache entries, calls of about 2 KB or more 0.0% (nxvideo:memset 17 calls of 51 instructions; nxvideo:memcpy 22 calls of 34 instructions; nano-X:memcpy 4 calls of 180 instructions). The kernel's `__memcpy` 1.47% (byte paths 0.03%, 64-byte word loop 0.01%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxvideo:media_video_open | 1 | 3,335,463 | 3,335,463 |
| nxvideo:m_mpeg_open | 1 | 3,334,004 | 3,334,004 |
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nano-X:nxPaintNCArea | 1 | 116,983 | 116,983 |

### video-avi

nxvideo: Big Buck Bunny, the AVI (MPEG-4). `NXVIDEO_ONCE=1 nxvideo "/root/Videos/Big Buck Bunny.avi"`

Window: `the command` to `client closed socket`: 6.0 s of the guest, 3,562 passes; core 0 14.7 M instructions (2.43 M a guest second, 4,128 a pass), all cores 364.8 M, 65 cores ran. Core 0 took 169,022 write-cache entries (66 distinct texels a pass that stored), and 38 of its passes ended full (6.3 a guest second); the workers' 11,342 (1876.6 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 10.2 | 19.3 | 94.4 |
| libc copy/fill/str | 0.1 | 0.0 | 0 |
| libc other | 0.1 | 0.1 | 0 |
| kernel | 41.4 | 56.7 | 0 |
| window server | 8.3 | 4.1 | 0 |
| unknown | 39.9 | 19.8 | 5.6 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x8167c000 | 10.6 | kernel:handle_exception | 15.5 |
| kernel:handle_exception | 9.2 | nxvideo:main | 14.8 |
| ?0x8167d000 | 8.8 | ?0x8168e000 | 3.8 |
| ?0x8167b000 | 8.5 | kernel:ktime_get_ts64 | 3.3 |
| nxvideo:main | 5.4 | ?0x8168d000 | 3.3 |
| kernel:ktime_get_ts64 | 4.5 | ?0x8167e000 | 3.2 |
| ?0x8169f000 | 3.9 | kernel:__memcpy | 2.8 |
| kernel:free_pcppages_bulk | 3.5 | kernel:__free_one_page | 2.1 |

Core 0's passes ended full, where (passes): nxvideo:main 9, ?0x8167b000 7, kernel:alloc_pages_run 6, kernel:handle_exception 3, kernel:__memcpy 2.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxvideo:mcu_pixels | 59.2 | nxvideo:mcu_pixels | 39.3 |
| nxvideo:idct.constprop.0 | 13.4 | nxvideo:m_jpeg_part | 37.4 |
| nxvideo:restart_intervals | 12.3 | nxvideo:restart_intervals | 7.5 |
| nxvideo:m_jpeg_part | 7.5 | nxvideo:idct.constprop.0 | 7.4 |
| ?0x8168c000 | 5.3 | ?0x8168c000 | 5.8 |
| nxvideo:huff_make | 1.4 | nxvideo:huff_make | 1.3 |

The workers' passes ended full, where (passes): nxvideo:m_jpeg_part 9328, nxvideo:mcu_pixels 745, nxvideo:restart_intervals 491, ?0x8168c000 455, nxvideo:idct.constprop.0 221.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 32,581 (0.22% of core 0, 209 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.04% of core 0's instructions and 0.17% of its write-cache entries, calls of about 2 KB or more 0.0% (nano-X:memcpy 63 calls of 111 instructions; nano-X:memset 10 calls of 86 instructions; nxvideo:memcpy 14 calls of 34 instructions). The kernel's `__memcpy` 1.97% (byte paths 0.12%, 64-byte word loop 0.13%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nano-X:epoll_wait | 35 | 144,471 | 478,888 |
| nano-X:GsSelect | 41 | 142,643 | 518,822 |
| nano-X:nxPaintNCArea | 1 | 116,983 | 116,983 |

### decode-avi

libmedia: the AVI decoded whole on 16 workers. `media_test video "/root/Videos/Big Buck Bunny.avi" 16`

Window: `the command` to `pictures' sum`: 62.1 s of the guest, 25,264 passes; core 0 183.7 M instructions (2.96 M a guest second, 7,272 a pass), all cores 531.8 M, 17 cores ran. Core 0 took 257,142 write-cache entries (67 distinct texels a pass that stored), and 4 of its passes ended full (0.1 a guest second); the workers' 11,808 (190.1 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 88.5 | 3.7 | 94.7 |
| libc other | 0.0 | 0.1 | 0.0 |
| kernel | 6.4 | 84.7 | 0 |
| unknown | 5.0 | 11.5 | 5.3 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| media_test:main | 50.1 | kernel:timekeeping_advance | 12.4 |
| media_test:media_sum | 38.3 | kernel:timekeeping_update | 11.2 |
| ?0x816b6000 | 3.2 | kernel:handle_exception | 9.4 |
| ?0x816b5000 | 1.6 | ?0x816b6000 | 6.9 |
| kernel:timekeeping_advance | 0.9 | kernel:tick_do_update_jiffies64 | 4.3 |
| kernel:alloc_pages_run | 0.8 | ?0x816b5000 | 3.3 |
| kernel:handle_exception | 0.5 | kernel:update_curr.constprop.0 | 2.7 |
| kernel:timekeeping_update | 0.4 | kernel:handle_percpu_devid_irq | 2.6 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 3, ?0x816b5000 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| media_test:mcu_pixels | 59.5 | media_test:mcu_pixels | 40.0 |
| media_test:idct.constprop.0 | 13.5 | media_test:m_jpeg_part | 37.9 |
| media_test:restart_intervals | 12.3 | media_test:idct.constprop.0 | 7.6 |
| media_test:m_jpeg_part | 7.4 | media_test:restart_intervals | 7.6 |
| ?0x816c1000 | 5.3 | ?0x816c1000 | 5.1 |
| media_test:huff_make | 1.4 | media_test:huff_make | 1.3 |

The workers' passes ended full, where (passes): media_test:m_jpeg_part 11267, ?0x816c1000 293, media_test:huff_make 88, media_test:restart_intervals 67, media_test:mcu_pixels 59.

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.0% of core 0's instructions and 0.01% of its write-cache entries, calls of about 2 KB or more 0.0% (media_test:memcpy 16 calls of 40 instructions; media_test:memset 8 calls of 48 instructions; media_test:memmove 1 calls of 25 instructions). The kernel's `__memcpy` 0.02% (byte paths 0.02%, 64-byte word loop 0.0%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| media_test:media_sum | 96 | 732,180 | 732,180 |
| media_test:media_video_open | 1 | 43,748 | 43,748 |
| media_test:m_avi_open | 1 | 41,874 | 41,874 |

### nxray

nxray: 320 x 240 on 255 workers. `nxray`

Window: `the command` to `the wall (50 s of the host)`: 2.4 s of the guest, 937 passes; core 0 3.3 M instructions (1.37 M a guest second, 3,526 a pass), all cores 1115.1 M, 256 cores ran. Core 0 took 45,897 write-cache entries (154 distinct texels a pass that stored), and 49 of its passes ended full (20.4 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 32.3 | 17.4 | 96.2 |
| libc copy/fill/str | 0.1 | 0.1 | 0 |
| libc other | 0.2 | 0.3 | 0 |
| kernel | 40.8 | 66.6 | 0 |
| window server | 8.2 | 7.4 | 0 |
| unknown | 18.4 | 8.3 | 3.8 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nxray:main | 27.4 | nxray:main | 12.8 |
| ?0x8148f000 | 12.6 | kernel:handle_exception | 11.1 |
| kernel:handle_exception | 6.2 | kernel:alloc_pages_run | 9.8 |
| kernel:alloc_pages_run | 4.1 | kernel:__memcpy | 3.4 |
| ?0x81480000 | 2.3 | kernel:timekeeping_advance | 2.6 |
| nano-X:gpu_drawstring | 2.2 | kernel:ktime_get_ts64 | 2.5 |
| kernel:ktime_get_ts64 | 2.2 | kernel:timekeeping_update | 2.4 |
| nxray:span | 1.9 | ?0x81480000 | 2.3 |

Core 0's passes ended full, where (passes): ?0x81480000 28, kernel:alloc_pages_run 18, kernel:handle_exception 1, kernel:do_wait 1, kernel:__memset 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxray:trace_texel | 95.2 | nxray:trace_texel | 92.5 |
| ?0x81478000 | 2.8 | nxray:worker_picture | 4.2 |
| ?0x81477000 | 0.9 | nxray:trace_texels | 3.2 |
| nxray:trace_texels | 0.8 | ?0x8147f000 | 0.1 |
| ?0x81481000 | 0.1 | nxray:trace_pictures | 0.0 |
| nxray:trace_pictures | 0.1 | ?0x81477000 | 0.0 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 3,198 (0.1% of core 0, 169 places).

Slow forms prof.py finds (% of core 0): 4.5%  [division-in-loop] nxray:main+0x122c  (2,794 trips of 53).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.16% of core 0's instructions and 0.6% of its write-cache entries, calls of about 2 KB or more 0.0% (nano-X:memcpy 57 calls of 114 instructions; nano-X:memset 9 calls of 86 instructions; nxray:memcpy 12 calls of 33 instructions). The kernel's `__memcpy` 1.57% (byte paths 0.45%, 64-byte word loop 0.18%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nxray:set_size | 1 | 495,369 | 495,369 |
| nano-X:epoll_wait | 42 | 215,642 | 750,157 |
| nano-X:GsSelect | 48 | 198,961 | 785,785 |

### nxpath

nxpath: the default scene, 160 x 120, 2 samples. `NXPATH_AUTO=1 NXPATH_EXIT=1 NXPATH_SIZE=160x120 NXPATH_SAMPLES=2 nxpath`

Window: `the command` to `pathstat:`: 4.3 s of the guest, 1,811 passes; core 0 13.3 M instructions (3.12 M a guest second, 7,336 a pass), all cores 75.1 M, 256 cores ran. Core 0 took 164,175 write-cache entries (163 distinct texels a pass that stored), and 169 of its passes ended full (39.6 a guest second); the workers' 371 (87.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 68.0 | 61.6 | 33.3 |
| libc copy/fill/str | 4.8 | 7.7 | 0 |
| libc other | 2.0 | 2.3 | 0 |
| libm | 0.1 | 0.0 | 0 |
| soft float, 64-bit | 1.2 | 0.1 | 0 |
| kernel | 16.4 | 25.0 | 0 |
| window server | 4.1 | 2.6 | 0 |
| window client | 0.7 | 0.6 | 0 |
| unknown | 2.7 | 0.1 | 66.7 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nxpath:sc_mesh_load | 25.4 | nxpath:sc_mesh_load | 23.8 |
| nxpath:show_mesh | 12.3 | nxpath:show_mesh | 13.4 |
| nxpath:number.constprop.0 | 11.6 | nxpath:memcpy | 7.4 |
| kernel:alloc_pages_run | 5.3 | nxpath:loaded_add | 6.4 |
| nxpath:memset | 3.4 | nxpath:_Z24ImGui_ImplShaderEmu_Initj | 5.0 |
| nxpath:grow | 3.4 | kernel:alloc_pages_run | 4.3 |
| nxpath:loaded_add | 3.2 | nxpath:sc_world_build | 2.4 |
| nxpath:fill_table | 2.1 | nxpath:grow | 2.4 |

Core 0's passes ended full, where (passes): nxpath:sc_mesh_load 45, nxpath:memcpy 31, nxpath:claim.isra.0 28, nxpath:_Z24ImGui_ImplShaderEmu_Initj 26, kernel:alloc_pages_run 24.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81620000 | 49.6 | ?0x81621000 | 26.2 |
| nxpath:world_between | 17.1 | ?0x81620000 | 21.3 |
| ?0x81621000 | 15.8 | nxpath:grow | 17.2 |
| nxpath:grow | 10.2 | nxpath:place_job | 16.5 |
| nxpath:place_job | 4.5 | nxpath:put_job | 14.5 |
| ?0x8161f000 | 1.0 | ?0x8161f000 | 2.1 |

The workers' passes ended full, where (passes): nxpath:put_job 267, nxpath:place_job 104.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 12,721 (0.1% of core 0, 167 places).

Slow forms prof.py finds (% of core 0): 2.3%  [division-in-loop] nxpath:number.constprop.0+0x58  (10,932 trips of 28); 2.2%  [division-in-loop] nxpath:number.constprop.0+0xb0  (10,932 trips of 27).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 4.41% of core 0's instructions and 7.6% of its write-cache entries, calls of about 2 KB or more 3.99% (nxpath:memset >=2KB 3 calls of 143,408 instructions; nxpath:memcpy >=2KB 10 calls of 9,970 instructions; nxpath:memcpy 1,198 calls of 34 instructions). The kernel's `__memcpy` 0.33% (byte paths 0.09%, 64-byte word loop 0.16%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): nxpath:__sindf 0.58, nxpath:__cosdf 0.56.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): ?0x81611000 2 3.2.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxpath:sc_mesh_load | 1 | 5,672,623 | 5,672,623 |
| nxpath:begin_trace | 1 | 1,195,176 | 1,195,176 |
| nxpath:sc_world_build | 1 | 929,797 | 929,797 |
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |

### nxweb

nxweb: its start page. `NXWEB_DUMP=1 nxweb`

Window: `the command` to `nxweb: /usr`: 0.6 s of the guest, 182 passes; core 0 1.2 M instructions (1.98 M a guest second, 6,576 a pass), all cores 1.3 M, 17 cores ran. Core 0 took 17,190 write-cache entries (134 distinct texels a pass that stored), and 10 of its passes ended full (16.5 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 30.8 | 18.5 | 93.2 |
| libc copy/fill/str | 5.5 | 2.8 | 6.2 |
| libc other | 7.3 | 6.8 | 0 |
| kernel | 51.9 | 69.0 | 0 |
| window server | 3.7 | 2.5 | 0 |
| window client | 0.2 | 0.4 | 0 |
| unknown | 0.6 | 0.1 | 0.6 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:alloc_pages_run | 14.1 | kernel:alloc_pages_run | 7.9 |
| nxweb:web_parse | 7.7 | kernel:__memcpy | 5.2 |
| nxweb:web_put_word | 3.7 | nxweb:web_new_node | 4.6 |
| kernel:load_flat_binary | 2.9 | kernel:handle_exception | 4.2 |
| nxweb:web_put_text | 2.7 | nxweb:web_parse | 2.6 |
| nxweb:css_atom | 2.6 | kernel:__memset | 2.6 |
| nxweb:web_word_end | 1.9 | nxweb:css_atom | 2.2 |
| kernel:romfs_lookup | 1.7 | kernel:timekeeping_advance | 1.6 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 4, kernel:__memcpy 2, kernel:kernel_waitid 1, kernel:account_system_index_time 1, kernel:do_dentry_open 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxweb:web_style_one | 46.9 | nxweb:web_style_one | 85.0 |
| nxweb:css_apply | 17.3 | ?0x81665000 | 4.6 |
| nxweb:web_face_for | 15.5 | nxweb:web_walk_to | 3.1 |
| nxweb:web_walk_to | 6.8 | nxweb:css_part_ok | 2.5 |
| nxweb:memset | 5.8 | nxweb:web_inside.isra.0 | 1.9 |
| nxweb:web_style_range | 2.3 | nxweb:css_apply | 1.2 |

Slow forms prof.py finds (% of core 0): 2.1%  [printf-in-hot-path] nxweb:vfprintf  (20 calls of 1,272).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.11% of core 0's instructions and 2.26% of its write-cache entries, calls of about 2 KB or more 0.0% (nxweb:memset 262 calls of 71 instructions; nxweb:memcpy 333 calls of 38 instructions; nano-X:memcpy 20 calls of 100 instructions). The kernel's `__memcpy` 1.38% (byte paths 0.37%, 64-byte word loop 0.61%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nxweb:go | 1 | 480,134 | 480,134 |
| nxweb:read_file | 1 | 477,925 | 477,925 |
| nxweb:show_kept | 1 | 475,791 | 475,791 |

### nxedit

nxedit: a text of 175 lines made in /tmp, opened to edit. `for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25; do echo \x22## Part $i\x22; echo; echo \x22Some *text* of part $i, with a [link](p$i`...

Window: 0 s after the command to 20 s later: 20.2 s of the guest, 1,513 passes; core 0 5.6 M instructions (0.28 M a guest second, 3,697 a pass), all cores 5.6 M, 1 cores ran. Core 0 took 91,563 write-cache entries (134 distinct texels a pass that stored), and 6 of its passes ended full (0.3 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 8.2 | 2.8 | 0 |
| libc copy/fill/str | 4.0 | 1.8 | 0 |
| libc other | 4.9 | 3.4 | 0 |
| kernel | 71.4 | 82.2 | 0 |
| window server | 7.9 | 4.8 | 0 |
| window client | 0.6 | 0.8 | 0 |
| unknown | 2.8 | 4.1 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:timekeeping_advance | 6.5 | kernel:timekeeping_advance | 7.8 |
| nano-X:gpu_drawstring | 3.0 | kernel:handle_exception | 7.4 |
| kernel:ktime_get | 3.0 | kernel:timekeeping_update | 7.1 |
| kernel:timekeeping_update | 2.8 | kernel:tick_irq_enter | 3.6 |
| kernel:__next_timer_interrupt | 2.6 | kernel:__run_timers.part.0 | 3.2 |
| kernel:_find_next_bit | 2.4 | kernel:tick_do_update_jiffies64 | 2.6 |
| kernel:free_pcppages_bulk | 2.1 | kernel:tick_nohz_handler | 2.4 |
| kernel:handle_exception | 2.1 | kernel:shaderemu_input_poll | 2.2 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 2, nano-X:__libc_malloc_impl 1, kernel:__memcpy 1, kernel:timekeeping_advance 1, kernel:rmqueue_bulk 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 5,695 (0.1% of core 0, 164 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.98% of core 0's instructions and 1.69% of its write-cache entries, calls of about 2 KB or more 0.0% (busybox:memcpy 878 calls of 40 instructions; busybox:memset 383 calls of 50 instructions; nxedit:memcpy 109 calls of 55 instructions). The kernel's `__memcpy` 0.78% (byte paths 0.36%, 64-byte word loop 0.23%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |

### nxide

nxide: started, pi.js opened and run. `NXIDE_PRINT=1 NXIDE_SCRIPT='example:pi run wait quit quit' nxide`

Window: `the command` to `the program ended`: 4.1 s of the guest, 1,800 passes; core 0 13.5 M instructions (3.29 M a guest second, 7,516 a pass), all cores 13.5 M, 1 cores ran. Core 0 took 192,944 write-cache entries (130 distinct texels a pass that stored), and 107 of its passes ended full (26.1 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 37.0 | 45.6 | 0 |
| libc copy/fill/str | 4.1 | 4.6 | 0 |
| libc other | 16.5 | 7.2 | 0 |
| soft float, 64-bit | 3.6 | 0.2 | 0 |
| kernel | 27.8 | 36.7 | 0 |
| window server | 7.0 | 4.1 | 0 |
| window client | 0.9 | 0.8 | 0 |
| unknown | 3.1 | 0.8 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nxide:printf_core | 8.3 | qjs:js_malloc_new_arena | 13.2 |
| ?0x816e1000 | 2.9 | kernel:handle_exception | 3.1 |
| nxide:memcpy_any | 2.7 | kernel:__memcpy | 3.0 |
| kernel:free_pcppages_bulk | 2.4 | qjs:__js_malloc | 2.8 |
| kernel:__memcpy | 2.0 | qjs:js_parse_expr_binary | 2.2 |
| kernel:load_flat_binary | 2.0 | qjs:memcpy | 1.8 |
| qjs:__JS_FindAtom.constprop.0 | 2.0 | nxide:memcpy | 1.7 |
| qjs:__js_malloc | 1.7 | kernel:__free_one_page | 1.7 |

Core 0's passes ended full, where (passes): kernel:handle_exception 9, qjs:js_malloc_new_arena 7, kernel:free_page_series 7, kernel:vmacache_find 6, kernel:__memcpy 5.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 18,851 (0.14% of core 0, 172 places).

Slow forms prof.py finds (% of core 0): 16.6%  [printf-in-hot-path] nxide:vfprintf  (2,237 calls of 1,003); 15.3%  [printf-in-hot-path] nxide:printf_core  (4,474 calls of 461); 1.1%  [byte-copy] vmlinux:__memcpy+0x17c  (30,305 trips of 5).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.41% of core 0's instructions and 4.66% of its write-cache entries, calls of about 2 KB or more 0.13% (nxide:memcpy 14,796 calls of 34 instructions; qjs:memcpy 2,366 calls of 48 instructions; nxide:memset 1,951 calls of 23 instructions). The kernel's `__memcpy` 2.04% (byte paths 1.12%, 64-byte word loop 0.36%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): qjs:js_bigint_to_string1 3.22, qjs:js_bigint_divrem 0.13.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nxide:command_done | 2 | 446,311 | 507,282 |
| nxide:build | 1 | 245,696 | 245,696 |
| nxide:tx_save | 1 | 238,440 | 238,440 |

### desktop

the desktop started, the Calculator launched from its icon. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey 266,94 266,94; sleep 4; echo DE""SK`

Window: `the command` to `DESK`: 14.7 s of the guest, 3,803 passes; core 0 26.8 M instructions (1.82 M a guest second, 7,057 a pass), all cores 66.7 M, 256 cores ran. Core 0 took 352,749 write-cache entries (163 distinct texels a pass that stored), and 109 of its passes ended full (7.4 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 3.9 | 1.7 | 0 |
| libc copy/fill/str | 4.7 | 2.1 | 3.7 |
| libc other | 6.9 | 4.9 | 0.5 |
| kernel | 70.9 | 81.0 | 0 |
| window server | 7.7 | 3.9 | 0 |
| window client | 0.5 | 0.8 | 0 |
| unknown | 5.3 | 5.6 | 95.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 10.7 | kernel:__free_one_page | 6.7 |
| kernel:__free_one_page | 5.8 | kernel:free_unref_page_commit.isra.0 | 4.3 |
| kernel:alloc_pages_run | 5.8 | kernel:handle_exception | 4.1 |
| kernel:load_flat_binary | 5.0 | kernel:alloc_pages_run | 4.1 |
| kernel:free_unref_page | 2.5 | kernel:free_page_series | 3.9 |
| nano-X:memset | 2.3 | kernel:__memcpy | 3.4 |
| kernel:free_unref_page_commit.isra.0 | 2.0 | kernel:__memset | 2.8 |
| kernel:__memcpy | 1.7 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 36, ?0x81724000 28, kernel:__memcpy 12, kernel:handle_exception 6, kernel:__free_one_page 5.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nxcalc:memset | 3.7 | nxcalc:memset | 8.6 |
| nxpersist:memchr | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 16,374 (0.06% of core 0, 313 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.97% of core 0's instructions and 1.86% of its write-cache entries, calls of about 2 KB or more 2.31% (nano-X:memset >=2KB 2 calls of 300,690 instructions; nxcalc:memset 1,944 calls of 100 instructions; nxbar:memcpy 7,116 calls of 25 instructions). The kernel's `__memcpy` 1.69% (byte paths 0.72%, 64-byte word loop 0.36%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 2.24.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |
| nxbar:GrGetScreenInfo | 2 | 286,796 | 570,615 |

### nxview

nxview: a JPEG picture opened. `NXVIEW_PRINT=1 nxview /root/Pictures/Earthrise.jpg`

Window: `the command` to `Earthrise.jpg, `: 1.8 s of the guest, 717 passes; core 0 5.1 M instructions (2.80 M a guest second, 7,174 a pass), all cores 232.2 M, 256 cores ran. Core 0 took 54,466 write-cache entries (109 distinct texels a pass that stored), and 50 of its passes ended full (27.2 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 15.7 | 8.1 | 17.4 |
| libc copy/fill/str | 1.3 | 0.8 | 0.3 |
| libc other | 1.5 | 2.0 | 0.0 |
| soft float, 64-bit | 9.9 | 0.3 | 82.2 |
| kernel | 60.6 | 77.2 | 0 |
| window server | 6.6 | 8.1 | 0 |
| window client | 0.8 | 1.5 | 0 |
| unknown | 3.6 | 1.9 | 0.1 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 12.8 | kernel:__free_one_page | 13.4 |
| kernel:__free_one_page | 10.7 | kernel:alloc_pages_run | 9.8 |
| nxview:__udivmoddi4 | 8.2 | kernel:free_unref_page_commit.isra.0 | 9.5 |
| kernel:alloc_pages_run | 6.7 | kernel:free_page_series | 8.7 |
| kernel:free_unref_page | 4.6 | kernel:handle_exception | 3.2 |
| nxview:mcw_done | 3.8 | ?0x81647000 | 1.7 |
| kernel:free_unref_page_commit.isra.0 | 3.7 | kernel:__memcpy | 1.7 |
| nxview:serve | 3.5 | kernel:timekeeping_advance | 1.7 |

Core 0's passes ended full, where (passes): nxview:claim.isra.0 28, kernel:alloc_pages_run 19, kernel:ovl_real_fdget_meta 1, kernel:workers_park 1, ?0x81647000 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxview:__udivmoddi4 | 71.4 | nxview:fit_rows | 36.1 |
| nxview:fit_rows | 11.8 | nxview:mcu_pixels | 26.0 |
| nxview:__clzsi2 | 6.6 | nxview:__divdi3 | 10.9 |
| nxview:__divdi3 | 4.2 | nxview:__udivmoddi4 | 10.9 |
| nxview:mcu_pixels | 4.0 | nxview:restart_intervals | 4.9 |
| nxview:restart_intervals | 0.8 | nxview:idct.constprop.0 | 4.0 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 4,586 (0.09% of core 0, 180 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 1.02% of core 0's instructions and 1.24% of its write-cache entries, calls of about 2 KB or more 0.63% (nxview:memset >=2KB 6 calls of 5,386 instructions; nxview:memcpy 389 calls of 35 instructions; nxview:memset 237 calls of 39 instructions). The kernel's `__memcpy` 0.56% (byte paths 0.27%, 64-byte word loop 0.12%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): nxview:fit_rows 9.87, nxview:__divdi3 8.93.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nxview:media_probe.part.0.constprop.0 3 0.38, nxview:media_probe 2 0.13, ?0x81642000 1 0.13.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxview:draw | 1 | 1,814,012 | 1,814,012 |
| nxview:media_image_decode_fit | 1 | 1,524,848 | 1,524,848 |
| nxview:media_image_fit | 1 | 1,114,030 | 1,114,030 |
| nxview:draw_room | 2 | 893,627 | 1,786,025 |
| nxview:ui_picture_decoded_at.constprop.0 | 2 | 892,728 | 1,785,126 |
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |

### nxpaint

nxpaint: a rectangle drawn, filled and taken back. `NXPAINT_SCRIPT="new:200x120 fg:ff0000 tool:Rect size:2 drag:20,20:120,90 sum tool:Fill fg:00ff00 press:60,50 pixel:60,50 undo sum quit quit" nxpaint`

Window: `the command` to `1 to make again`: 1.2 s of the guest, 478 passes; core 0 3.4 M instructions (2.81 M a guest second, 7,028 a pass), all cores 5.3 M, 97 cores ran. Core 0 took 44,879 write-cache entries (132 distinct texels a pass that stored), and 57 of its passes ended full (47.7 a guest second); the workers' 848 (710.2 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 45.2 | 25.0 | 73.0 |
| libc copy/fill/str | 1.9 | 3.6 | 26.6 |
| libc other | 3.4 | 3.1 | 0 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 29.2 | 51.1 | 0 |
| window server | 17.6 | 13.0 | 0 |
| window client | 2.5 | 4.2 | 0 |
| unknown | 0.0 | 0.0 | 0.4 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nxpaint:draw_tools | 16.4 | nxpaint:turn | 11.1 |
| nxpaint:scripted | 11.5 | kernel:alloc_pages_run | 9.3 |
| kernel:alloc_pages_run | 6.7 | kernel:__memcpy | 7.9 |
| nxpaint:field_show | 5.5 | nxpaint:draw_at | 3.9 |
| nano-X:gpu_drawstring | 2.3 | kernel:handle_exception | 3.0 |
| nxpaint:turn | 2.0 | nxpaint:field_show | 3.0 |
| nxpaint:serve | 1.6 | nxpaint:memcpy | 2.9 |
| kernel:free_pcppages_bulk | 1.6 | nxpaint:span.part.0 | 2.1 |

Core 0's passes ended full, where (passes): nxpaint:turn 18, kernel:alloc_pages_run 17, nxpaint:claim.isra.0 10, kernel:__memcpy 8, nxpaint:span.part.0 1.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxpaint:bulk_plain | 71.8 | nxpaint:bulk_plain | 66.7 |
| nxpaint:memcpy | 26.6 | nxpaint:memcpy | 32.2 |
| nxpaint:bulk_work | 1.2 | nxpaint:bulk_work | 0.8 |
| ?0x81658000 | 0.4 | ?0x81658000 | 0.3 |

The workers' passes ended full, where (passes): nxpaint:bulk_plain 745, nxpaint:memcpy 97, ?0x81658000 6.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 12,443 (0.37% of core 0, 175 places).

Slow forms prof.py finds (% of core 0): 5.0%  [division-in-loop] nxpaint:field_show+0xfc  (5,280 trips of 32).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 1.44% of core 0's instructions and 4.08% of its write-cache entries, calls of about 2 KB or more 0.0% (nxpaint:memcpy 595 calls of 41 instructions; nxpaint:memset 176 calls of 89 instructions; nano-X:memcpy 58 calls of 108 instructions). The kernel's `__memcpy` 1.49% (byte paths 0.33%, 64-byte word loop 0.88%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxpaint:draw_all | 1 | 679,535 | 679,535 |
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nxpaint:draw_tools | 3 | 225,908 | 246,436 |
| nxpaint:key | 2 | 221,540 | 221,551 |

### nxfiles

nxfiles: /usr/share opened. `NXFILES_PRINT=1 nxfiles /usr/share`

Window: 0 s after the command to 20 s later: 20.2 s of the guest, 1,447 passes; core 0 4.9 M instructions (0.24 M a guest second, 3,368 a pass), all cores 4.9 M, 1 cores ran. Core 0 took 76,394 write-cache entries (123 distinct texels a pass that stored), and 5 of its passes ended full (0.2 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 0.4 | 0.2 | 0 |
| libc copy/fill/str | 7.3 | 2.5 | 0 |
| libc other | 2.7 | 2.7 | 0 |
| kernel | 77.3 | 87.1 | 0 |
| window server | 6.7 | 4.8 | 0 |
| window client | 0.5 | 0.7 | 0 |
| unknown | 5.1 | 2.0 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:timekeeping_advance | 7.6 | kernel:timekeeping_advance | 9.4 |
| nxfiles:strcasecmp | 3.9 | kernel:timekeeping_update | 8.6 |
| ?0x8194e000 | 3.6 | kernel:handle_exception | 7.0 |
| kernel:ktime_get | 3.5 | kernel:tick_irq_enter | 4.4 |
| kernel:timekeeping_update | 3.2 | kernel:__run_timers.part.0 | 3.9 |
| kernel:__next_timer_interrupt | 3.0 | kernel:tick_do_update_jiffies64 | 3.1 |
| kernel:_find_next_bit | 2.8 | kernel:tick_nohz_handler | 2.9 |
| kernel:free_pcppages_bulk | 2.4 | kernel:shaderemu_input_poll | 2.7 |

Core 0's passes ended full, where (passes): kernel:__memcpy 1, kernel:do_mmap 1, kernel:call_rcu 1, kernel:__free_one_page 1, nano-X:GsHandleClient 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 3,760 (0.08% of core 0, 154 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.36% of core 0's instructions and 0.73% of its write-cache entries, calls of about 2 KB or more 0.0% (nxfiles:memcpy 867 calls of 71 instructions; nxfiles:memset 75 calls of 75 instructions; nano-X:memcpy 38 calls of 128 instructions). The kernel's `__memcpy` 2.35% (byte paths 1.21%, 64-byte word loop 0.23%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,360 | 652,360 |
| nano-X:GdOpenScreen | 1 | 609,440 | 609,440 |
| nano-X:gpu_open | 1 | 607,712 | 607,712 |
| nano-X:nxPaintNCArea | 1 | 117,343 | 117,343 |
| nano-X:caption_title | 1 | 71,324 | 71,324 |
| nano-X:GrText | 1 | 70,788 | 70,788 |

### memory

memb.c on one core (cc in the machine): sequential, byte, random stores and loads, libc's copy and fill. `cat > /tmp/m.c\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <time.h>\ndouble now(){struct timespec t;clock_gettime(1,&t);return t.tv_`...

Window: `the command` to `memb: finished`: 29.9 s of the guest, 50,339 passes; core 0 64.0 M instructions (2.14 M a guest second, 1,272 a pass), all cores 64.0 M, 1 cores ran. Core 0 took 2,292,579 write-cache entries (227 distinct texels a pass that stored), and 5,966 of its passes ended full (199.4 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 11.8 | 3.2 | 0 |
| libc copy/fill/str | 8.2 | 3.9 | 0 |
| libc other | 1.8 | 1.0 | 0 |
| soft float, 64-bit | 0.4 | 0.2 | 0 |
| kernel | 15.6 | 8.4 | 0 |
| window server | 0.2 | 0.0 | 0 |
| unknown | 61.9 | 83.3 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x81478000 | 42.7 | ?0x81483000 | 57.2 |
| ?0x81483000 | 10.2 | ?0x81478000 | 20.2 |
| ?0x81477000 | 8.4 | ?0x81477000 | 5.8 |
| tcc:memcpy | 5.8 | tcc:memcpy | 3.6 |
| tcc:next_nomacro | 2.2 | kernel:timekeeping_advance | 0.7 |
| kernel:alloc_pages_run | 1.4 | kernel:timekeeping_update | 0.6 |
| tcc:tcc_load_alacarte | 1.4 | kernel:__memcpy | 0.5 |
| kernel:timekeeping_advance | 1.2 | kernel:handle_exception | 0.4 |

Core 0's passes ended full, where (passes): ?0x81483000 4129, ?0x81478000 1097, ?0x81477000 406, tcc:memcpy 235, kernel:alloc_pages_run 21.

Slow forms prof.py finds (% of core 0): 11.5%  [independent-pixel-loop] ?0x81478000  (262,144 trips of 28); 10.2%  [independent-pixel-loop] ?0x81483000  (655,359 trips of 10); 5.3%  [independent-pixel-loop] ?0x81478000  (131,072 trips of 26); 4.9%  [independent-pixel-loop] ?0x81478000  (131,075 trips of 24); 4.9%  [independent-pixel-loop] ?0x81478000  (131,072 trips of 24).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 6.25% of core 0's instructions and 3.9% of its write-cache entries, calls of about 2 KB or more 5.48% (tcc:memcpy >=2KB 267 calls of 13,060 instructions; tcc:memcpy 4,905 calls of 94 instructions; tcc:memset 3,523 calls of 60 instructions). The kernel's `__memcpy` 0.3% (byte paths 0.06%, 64-byte word loop 0.16%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| tcc:decl | 1 | 5,436,382 | 5,436,382 |
| tcc:tcc_add_file | 5 | 1,353,595 | 5,869,227 |
| tcc:tcc_output_file | 1 | 1,096,154 | 1,096,154 |

### memory-kernel

the kernel's copy: 32 MB from /dev/zero, 8 MB into a file of the memory's file system, it read 4 times. `dd if=/dev/zero of=/dev/null bs=64k count=512 2>&1 | tail -1; echo DD""1; dd if=/dev/zero of=/tmp/z bs=64k count=128 2>&1 | tail -1; echo DD""2; cat /tmp/z /tmp`...

Window: `the command` to `DD3`: 6.5 s of the guest, 13,161 passes; core 0 12.0 M instructions (1.84 M a guest second, 915 a pass), all cores 12.0 M, 1 cores ran. Core 0 took 213,760 write-cache entries (73 distinct texels a pass that stored), and 6 of its passes ended full (0.9 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 2.4 | 2.6 | 0 |
| libc copy/fill/str | 0.3 | 0.0 | 0 |
| libc other | 0.2 | 0.4 | 0 |
| kernel | 95.8 | 96.0 | 0 |
| window server | 1.0 | 0.9 | 0 |
| unknown | 0.2 | 0.1 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:copy_page_to_iter | 8.4 | kernel:rmqueue_bulk | 10.5 |
| kernel:filemap_read | 5.4 | kernel:get_page_from_freelist | 5.8 |
| kernel:free_pcppages_bulk | 4.7 | kernel:filemap_get_read_batch | 5.6 |
| kernel:filemap_get_read_batch | 4.6 | kernel:__alloc_pages | 5.6 |
| kernel:__splice_from_pipe | 4.5 | kernel:copy_page_to_iter | 5.0 |
| kernel:load_flat_binary | 3.1 | kernel:__filemap_add_folio | 4.8 |
| kernel:get_page_from_freelist | 2.8 | kernel:__filemap_get_folio | 4.5 |
| kernel:rmqueue_bulk | 2.5 | kernel:simple_write_end | 3.8 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 6.

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.09% of core 0's instructions and 0.1% of its write-cache entries, calls of about 2 KB or more 0.0% (nano-X:memcpy 262 calls of 31 instructions; nano-X:memset 91 calls of 63 instructions; busybox:memmove 2 calls of 306 instructions). The kernel's `__memcpy` 0.45% (byte paths 0.07%, 64-byte word loop 0.05%).

### cc

cc (tcc): an example compiled and linked. `cd /tmp; cc /usr/share/example-cube.c -o cube.x; echo CC""DONE`

Window: `the command` to `CCDONE`: 7.9 s of the guest, 3,476 passes; core 0 26.0 M instructions (3.29 M a guest second, 7,467 a pass), all cores 26.0 M, 1 cores ran. Core 0 took 345,931 write-cache entries (130 distinct texels a pass that stored), and 294 of its passes ended full (37.3 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 52.9 | 39.6 | 0 |
| libc copy/fill/str | 16.9 | 16.5 | 0 |
| libc other | 7.9 | 11.8 | 0 |
| soft float, 64-bit | 0.3 | 0.0 | 0 |
| kernel | 21.5 | 31.7 | 0 |
| unknown | 0.4 | 0.4 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| tcc:next_nomacro | 10.4 | tcc:memcpy | 10.1 |
| tcc:strcmp | 8.5 | kernel:__memcpy | 6.8 |
| tcc:tcc_load_object_file | 4.8 | tcc:memcpy_any | 5.2 |
| kernel:alloc_pages_run | 4.6 | tcc:tcc_load_object_file | 5.0 |
| tcc:memcpy_any | 3.5 | tcc#2:memcpy | 3.2 |
| tcc:tcc_load_alacarte | 3.3 | tcc:tok_alloc_new | 2.8 |
| tcc:put_elf_sym | 2.7 | tcc:memset | 2.8 |
| kernel:free_pcppages_bulk | 2.4 | tcc:put_elf_sym | 2.7 |

Core 0's passes ended full, where (passes): tcc:memcpy 78, kernel:__memcpy 50, tcc#2:memcpy 35, tcc:tcc_write_elf_file 22, tcc:relocate_syms 20.

Slow forms prof.py finds (% of core 0): 8.5%  [many-small-copies] tcc:strcmp  (104,514 calls of 21).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 4.23% of core 0's instructions and 16.12% of its write-cache entries, calls of about 2 KB or more 0.89% (tcc:memcpy 8,367 calls of 114 instructions; tcc:memmove 6,827 calls of 67 instructions; tcc:memset 5,868 calls of 62 instructions). The kernel's `__memcpy` 1.07% (byte paths 0.06%, 64-byte word loop 0.79%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| tcc:decl | 1 | 7,925,869 | 7,925,869 |
| tcc:tcc_output_file | 1 | 2,870,102 | 2,870,102 |
| tcc:tcc_add_file | 5 | 1,836,323 | 8,444,407 |

### qjs

qjs: 200 digits of pi. `qjs /examples/pi.js 200; echo QJ""DONE`

Window: `the command` to `QJDONE`: 1.2 s of the guest, 525 passes; core 0 4.1 M instructions (3.31 M a guest second, 7,811 a pass), all cores 4.1 M, 1 cores ran. Core 0 took 57,209 write-cache entries (122 distinct texels a pass that stored), and 16 of its passes ended full (12.9 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 60.3 | 64.2 | 0 |
| libc copy/fill/str | 4.0 | 5.6 | 0 |
| libc other | 3.9 | 4.9 | 0 |
| soft float, 64-bit | 11.4 | 0.2 | 0 |
| kernel | 19.7 | 24.5 | 0 |
| window server | 0.2 | 0.0 | 0 |
| unknown | 0.4 | 0.5 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| qjs:__udivdi3 | 5.1 | qjs:js_malloc_new_arena | 9.7 |
| qjs:__umoddi3 | 4.7 | qjs:__js_malloc | 5.1 |
| qjs:js_bigint_divrem | 3.5 | qjs:memcpy | 4.4 |
| qjs:__JS_FindAtom.constprop.0 | 3.1 | qjs:JS_CallInternal | 3.3 |
| qjs:__js_malloc | 3.1 | qjs:js_parse_expr_binary | 3.3 |
| kernel:load_flat_binary | 2.8 | qjs:__js_free | 2.9 |
| kernel:free_pcppages_bulk | 2.8 | qjs:__JS_NewAtom | 2.6 |
| qjs:JS_CallInternal | 2.4 | qjs:add_shape_property | 2.5 |

Core 0's passes ended full, where (passes): qjs:js_malloc_new_arena 6, kernel:kernel_waitid 1, kernel:enqueue_task_fair 1, kernel:tick_nohz_handler 1, kernel:__memcpy 1.

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.03% of core 0's instructions and 5.59% of its write-cache entries, calls of about 2 KB or more 0.0% (qjs:memcpy 2,120 calls of 46 instructions; qjs:memset 517 calls of 50 instructions; qjs:memmove 4 calls of 444 instructions). The kernel's `__memcpy` 0.21% (byte paths 0.04%, 64-byte word loop 0.11%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): qjs:js_bigint_to_string1 10.64, qjs:js_bigint_divrem 0.44.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| qjs:eval_file | 1 | 2,063,102 | 2,063,102 |
| qjs:eval_buf | 1 | 2,059,285 | 2,059,285 |
| qjs:JS_Eval | 1 | 2,059,267 | 2,059,267 |
| qjs:JS_EvalFunctionInternal | 1 | 1,183,824 | 1,183,824 |
| qjs:JS_CallInternal | 1 | 1,145,491 | 1,145,491 |
| qjs:JS_NewCustomContext | 1 | 790,417 | 790,417 |

### python

micropython: a sum of 20,000 squares (big integers past 2^31). `micropython -c 'print(sum(i*i for i in range(20000)))'; echo PY""DONE`

Window: `the command` to `PYDONE`: 7.2 s of the guest, 2,929 passes; core 0 23.9 M instructions (3.32 M a guest second, 8,177 a pass), all cores 23.9 M, 1 cores ran. Core 0 took 138,911 write-cache entries (58 distinct texels a pass that stored), and 4 of its passes ended full (0.6 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 86.3 | 77.4 | 0 |
| libc copy/fill/str | 6.0 | 0.1 | 0 |
| libc other | 0.1 | 0.2 | 0 |
| kernel | 7.6 | 22.2 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| micropython:mp_execute_bytecode | 21.5 | micropython:mp_obj_malloc_helper | 13.4 |
| micropython:gc_alloc | 12.8 | micropython:mpn_add | 13.3 |
| micropython:mp_binary_op | 8.5 | micropython:gc_alloc | 10.3 |
| micropython:memset | 5.9 | micropython:mp_execute_bytecode | 9.1 |
| micropython:mp_obj_int_binary_op | 5.3 | micropython:mp_obj_gen_resume | 6.5 |
| micropython:mp_obj_gen_resume | 4.5 | micropython:m_malloc | 4.0 |
| micropython:mpz_add_inpl | 4.5 | micropython:nlr_push | 3.0 |
| micropython:mpn_add | 3.7 | kernel:timekeeping_advance | 2.7 |

Core 0's passes ended full, where (passes): kernel:__memset 3, kernel:alloc_pages_run 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): micropython 333,023 (1.39% of core 0, 79 places).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.0% of core 0's instructions and 0.0% of its write-cache entries, calls of about 2 KB or more 0.0% (). The kernel's `__memcpy` 0.02% (byte paths 0.01%, 64-byte word loop 0.01%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| micropython:pyexec_vstr | 1 | 22,063,043 | 22,063,043 |
| micropython:mp_call_function_0 | 1 | 21,953,564 | 21,953,564 |
| micropython:mp_execute_bytecode | 1 | 21,953,301 | 21,953,301 |
| micropython:mp_call_function_n_kw | 4 | 5,487,712 | 21,947,951 |
| micropython:mp_parse | 1 | 82,540 | 82,540 |

### gzip

gzip: seq 1 20000 (109 KB of text) made, then gzip three times. `echo SE""Q; seq 1 20000 > /tmp/g; echo GZ""GO; i=0; while [ $i -lt 3 ]; do gzip -c /tmp/g > /dev/null; i=$((i+1)); done; echo GZ""DONE`

Window: `the command` to `GZDONE`: 65.0 s of the guest, 26,504 passes; core 0 216.1 M instructions (3.32 M a guest second, 8,154 a pass), all cores 216.1 M, 1 cores ran. Core 0 took 1,084,273 write-cache entries (53 distinct texels a pass that stored), and 55 of its passes ended full (0.8 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 61.2 | 42.3 | 0 |
| libc copy/fill/str | 0.7 | 1.2 | 0 |
| libc other | 14.0 | 19.9 | 0 |
| libm | 0.6 | 1.7 | 0 |
| soft float, 64-bit | 17.7 | 12.0 | 0 |
| kernel | 5.7 | 22.9 | 0 |
| window server | 0.1 | 0.2 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| busybox:deflate | 47.3 | busybox:deflate | 24.6 |
| busybox:__multf3 | 7.8 | busybox:ct_tally | 10.6 |
| busybox:fmt_fp | 5.2 | busybox:fmt_fp | 6.6 |
| busybox:printf_core | 4.3 | busybox:printf_core | 5.2 |
| busybox:fill_window_if_needed | 3.3 | busybox:fill_window_if_needed | 5.1 |
| busybox:ct_tally | 2.9 | busybox:__multf3 | 4.6 |
| busybox:send_bits | 2.6 | kernel:timekeeping_advance | 3.1 |
| busybox:__addtf3 | 2.1 | kernel:timekeeping_update | 2.8 |

Core 0's passes ended full, where (passes): busybox:memcpy 35, kernel:__memset 8, kernel:alloc_pages_run 7, kernel:rmqueue_bulk 1, kernel:handle_exception 1.

Slow forms prof.py finds (% of core 0): 39.8%  [independent-pixel-loop] busybox:deflate+0x248  (6,611,133 trips of 13); 29.6%  [printf-in-hot-path] busybox:printf  (20,000 calls of 3,196); 29.4%  [printf-in-hot-path] busybox:vfprintf  (20,000 calls of 3,177); 28.7%  [printf-in-hot-path] busybox:printf_core  (40,000 calls of 1,551); 1.7%  [crc-bytewise] busybox:crc32_block_endian0+0x4  (326,682 trips of 11).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 0.56% of core 0's instructions and 1.18% of its write-cache entries, calls of about 2 KB or more 0.16% (busybox:memcpy 119,894 calls of 25 instructions; busybox:memset >=2KB 24 calls of 10,267 instructions; busybox:memcpy >=2KB 6 calls of 17,932 instructions). The kernel's `__memcpy` 0.24% (byte paths 0.23%, 64-byte word loop 0.01%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): busybox:fmt_fp 13.01, busybox:seq_main 2.76, busybox:pop_arg 0.44.

### htop

htop: started, quit by q once its keys are drawn. `htop -d 5; echo HT""DONE`

Window: `the command` to `HTDONE`: 2.7 s of the guest, 1,056 passes; core 0 8.3 M instructions (3.12 M a guest second, 7,854 a pass), all cores 8.3 M, 1 cores ran. Core 0 took 101,279 write-cache entries (152 distinct texels a pass that stored), and 35 of its passes ended full (13.2 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 12.8 | 17.0 | 0 |
| libc copy/fill/str | 23.3 | 6.7 | 0 |
| libc other | 16.2 | 13.3 | 0 |
| libm | 0.1 | 0.1 | 0 |
| soft float, 64-bit | 1.6 | 0.4 | 0 |
| kernel | 45.9 | 62.6 | 0 |
| window server | 0.1 | 0.0 | 0 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| htop:memcmp | 14.6 | htop:newwin_sp | 5.7 |
| htop:memset | 4.7 | htop:memset | 4.6 |
| htop:printf_core | 2.3 | kernel:handle_exception | 4.5 |
| kernel:__memcpy | 2.0 | kernel:__memcpy | 4.5 |
| kernel:handle_exception | 2.0 | kernel:__memset | 3.5 |
| kernel:load_flat_binary | 1.9 | htop:doupdate_sp | 1.9 |
| htop:memcpy_any | 1.6 | kernel:inode_init_always | 1.4 |
| htop:vfscanf | 1.6 | htop:__libc_malloc_impl | 1.4 |

Core 0's passes ended full, where (passes): htop:newwin_sp 13, htop:doupdate_sp 6, kernel:rmqueue_bulk 2, htop:_nc_makenew_sp 2, htop:alloc_slot 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): htop 1,629 (0.02% of core 0, 217 places).

Slow forms prof.py finds (% of core 0): 5.4%  [printf-in-hot-path] htop:vfprintf  (434 calls of 1,034); 4.6%  [printf-in-hot-path] htop:printf_core  (868 calls of 437); 2.4%  [printf-in-hot-path] htop:vsnprintf  (139 calls of 1,448); 2.1%  [printf-in-hot-path] vmlinux:vsnprintf  (294 calls of 595); 1.1%  [byte-copy] vmlinux:__memcpy+0x17c  (18,226 trips of 5).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 5.56% of core 0's instructions and 5.76% of its write-cache entries, calls of about 2 KB or more 0.0% (htop:memset 4,977 calls of 56 instructions; htop:memcpy 4,458 calls of 42 instructions; nano-X:memmove 3 calls of 25 instructions). The kernel's `__memcpy` 2.02% (byte paths 1.1%, 64-byte word loop 0.37%).

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| htop:ScreenManager_run | 1 | 2,227,234 | 2,227,234 |
| htop:Panel_getCh | 1 | 1,664,678 | 1,664,678 |
| htop:_nc_wgetch | 1 | 1,664,611 | 1,664,611 |
| htop:wrefresh | 2 | 1,010,348 | 1,664,188 |
| htop:doupdate_sp | 2 | 892,365 | 1,431,294 |
| htop:CRT_init | 1 | 676,348 | 676,348 |

### tetris

Tetris: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 g 300 enter 300 t 300 enter; sleep 8; ps | grep -v grep | grep "nxtetris" | sed "s/`...

Window: `the command` to `APPDONE`: 21.7 s of the guest, 4,663 passes; core 0 31.7 M instructions (1.46 M a guest second, 6,789 a pass), all cores 71.5 M, 256 cores ran. Core 0 took 414,986 write-cache entries (157 distinct texels a pass that stored), and 111 of its passes ended full (5.1 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 3.5 | 1.7 | 0 |
| libc copy/fill/str | 2.8 | 1.4 | 0 |
| libc other | 9.6 | 7.5 | 0.0 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 71.3 | 81.1 | 0 |
| window server | 8.6 | 4.4 | 4.1 |
| window client | 0.3 | 0.5 | 0 |
| unknown | 3.8 | 3.4 | 95.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 10.4 | kernel:__free_one_page | 6.4 |
| kernel:alloc_pages_run | 5.6 | kernel:handle_exception | 4.3 |
| kernel:__free_one_page | 5.6 | kernel:free_unref_page_commit.isra.0 | 4.2 |
| kernel:load_flat_binary | 5.1 | kernel:alloc_pages_run | 3.9 |
| nano-X:memset | 3.0 | kernel:free_page_series | 3.7 |
| kernel:free_unref_page | 2.4 | kernel:__memcpy | 2.9 |
| kernel:free_unref_page_commit.isra.0 | 1.9 | kernel:__memset | 2.6 |
| nxtetris:vfscanf | 1.8 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 41, ?0x81724000 28, kernel:handle_exception 6, kernel:__memcpy 6, kernel:__memset 4.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nano-X:memset | 3.7 | nano-X:memset | 8.6 |
| nano-X:memchr | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.85% of core 0's instructions and 2.14% of its write-cache entries, calls of about 2 KB or more 1.96% (nano-X:memset >=2KB 5 calls of 124,150 instructions; nano-X:memset 3,241 calls of 97 instructions; nxbar:memcpy 7,315 calls of 25 instructions). The kernel's `__memcpy` 1.53% (byte paths 0.65%, 64-byte word loop 0.31%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.9.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,421 | 652,421 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |

### mines

Mines: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 g 300 enter 300 m 300 enter; sleep 8; ps | grep -v grep | grep "nxmine" | sed "s/^/`...

Window: `the command` to `APPDONE`: 21.8 s of the guest, 4,756 passes; core 0 32.4 M instructions (1.49 M a guest second, 6,808 a pass), all cores 72.3 M, 256 cores ran. Core 0 took 421,475 write-cache entries (156 distinct texels a pass that stored), and 109 of its passes ended full (5.0 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 3.6 | 1.7 | 0 |
| libc copy/fill/str | 3.3 | 1.4 | 0 |
| libc other | 7.8 | 7.3 | 0.0 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 69.8 | 79.9 | 0 |
| window server | 11.7 | 5.8 | 4.1 |
| window client | 0.4 | 0.7 | 0 |
| unknown | 3.3 | 3.1 | 95.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 10.2 | kernel:__free_one_page | 6.4 |
| kernel:alloc_pages_run | 5.6 | kernel:handle_exception | 4.3 |
| kernel:__free_one_page | 5.5 | kernel:free_unref_page_commit.isra.0 | 4.1 |
| kernel:load_flat_binary | 5.0 | kernel:alloc_pages_run | 3.9 |
| nano-X:memset | 2.9 | kernel:free_page_series | 3.7 |
| kernel:free_unref_page | 2.3 | kernel:__memcpy | 2.8 |
| kernel:free_unref_page_commit.isra.0 | 1.9 | kernel:__memset | 2.4 |
| nano-X:vfscanf | 1.8 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 43, ?0x81724000 28, kernel:handle_exception 6, kernel:__memcpy 6, kernel:handle_percpu_devid_irq 3.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nano-X:memset | 3.7 | nano-X:memset | 8.6 |
| nano-X:memchr | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.75% of core 0's instructions and 2.07% of its write-cache entries, calls of about 2 KB or more 1.92% (nano-X:memset >=2KB 5 calls of 124,150 instructions; nano-X:memset 3,225 calls of 97 instructions; nxbar:memcpy 7,304 calls of 25 instructions). The kernel's `__memcpy` 1.48% (byte paths 0.63%, 64-byte word loop 0.29%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.86.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |

### terminal

the Terminal: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 u 300 enter 300 t 300 enter; sleep 8; ps | grep -v grep | grep "nxterm" | sed "s/^/`...

Window: `the command` to `APPDONE`: 21.9 s of the guest, 4,781 passes; core 0 32.4 M instructions (1.48 M a guest second, 6,781 a pass), all cores 72.3 M, 256 cores ran. Core 0 took 432,471 write-cache entries (160 distinct texels a pass that stored), and 115 of its passes ended full (5.3 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 4.2 | 2.5 | 0 |
| libc copy/fill/str | 4.0 | 2.2 | 3.7 |
| libc other | 7.8 | 7.2 | 0.0 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 71.2 | 80.3 | 0 |
| window server | 8.9 | 4.0 | 0.4 |
| window client | 0.3 | 0.5 | 0 |
| unknown | 3.5 | 3.2 | 95.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 9.9 | kernel:__free_one_page | 6.2 |
| kernel:alloc_pages_run | 5.8 | kernel:handle_exception | 4.4 |
| kernel:__free_one_page | 5.5 | kernel:free_unref_page_commit.isra.0 | 4.0 |
| kernel:load_flat_binary | 5.2 | kernel:alloc_pages_run | 3.9 |
| kernel:free_unref_page | 2.3 | kernel:free_page_series | 3.6 |
| nano-X:memset | 2.3 | kernel:__memcpy | 2.8 |
| kernel:free_unref_page_commit.isra.0 | 1.9 | kernel:__memset | 2.5 |
| nano-X:vfscanf | 1.8 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 43, ?0x81724000 28, kernel:handle_exception 6, kernel:__memcpy 5, nxterm:paint_frame 4.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nxterm:memset | 3.7 | nxterm:memset | 8.6 |
| nano-X:memchr | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.83% of core 0's instructions and 2.4% of its write-cache entries, calls of about 2 KB or more 1.91% (nano-X:memset >=2KB 2 calls of 300,690 instructions; nxterm:memset 1,906 calls of 101 instructions; nxbar:memcpy 7,376 calls of 25 instructions). The kernel's `__memcpy` 1.52% (byte paths 0.64%, 64-byte word loop 0.3%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.86.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |

### clock

the Clock: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 u 300 enter 300 c 300 c 300 enter; sleep 8; ps | grep -v grep | grep "nxclock" | se`...

Window: `the command` to `APPDONE`: 22.4 s of the guest, 5,029 passes; core 0 34.7 M instructions (1.55 M a guest second, 6,896 a pass), all cores 74.6 M, 256 cores ran. Core 0 took 441,856 write-cache entries (153 distinct texels a pass that stored), and 111 of its passes ended full (5.0 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 3.3 | 1.6 | 0 |
| libc copy/fill/str | 2.9 | 1.4 | 0 |
| libc other | 8.8 | 7.0 | 0.0 |
| soft float, 64-bit | 0.1 | 0.1 | 0 |
| kernel | 66.7 | 78.8 | 0 |
| window server | 14.4 | 7.2 | 4.1 |
| window client | 0.3 | 0.5 | 0 |
| unknown | 3.6 | 3.4 | 95.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 9.6 | kernel:__free_one_page | 6.1 |
| kernel:alloc_pages_run | 5.3 | kernel:handle_exception | 4.4 |
| kernel:__free_one_page | 5.2 | kernel:free_unref_page_commit.isra.0 | 3.9 |
| kernel:load_flat_binary | 4.7 | kernel:alloc_pages_run | 3.7 |
| nano-X:memset | 2.7 | kernel:free_page_series | 3.5 |
| kernel:free_unref_page | 2.2 | kernel:__memcpy | 2.8 |
| kernel:free_unref_page_commit.isra.0 | 1.8 | kernel:__memset | 2.6 |
| nxclock:vfscanf | 1.7 | kernel:load_flat_binary | 2.3 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 41, ?0x81724000 27, kernel:handle_exception 8, kernel:__memcpy 6, kernel:__memset 3.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nano-X:memset | 3.7 | nano-X:memset | 8.6 |
| nano-X:memchr | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

Slow forms prof.py finds (% of core 0): 1.5%  [software-float] nano-X:__adddf3  (3,645 calls of 145).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.56% of core 0's instructions and 2.13% of its write-cache entries, calls of about 2 KB or more 1.79% (nano-X:memset >=2KB 5 calls of 124,150 instructions; nano-X:memset 3,208 calls of 97 instructions; nxbar:memcpy 7,315 calls of 25 instructions). The kernel's `__memcpy` 1.44% (byte paths 0.59%, 64-byte word loop 0.3%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): nano-X:GdFillPoly 1.79, nano-X:wrapper_cmp 0.14.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.73.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |

### monitor

the Monitor: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 u 300 enter 300 m 300 enter; sleep 8; ps | grep -v grep | grep "nxmon" | sed "s/^/R`...

Window: `the command` to `APPDONE`: 21.9 s of the guest, 5,385 passes; core 0 35.2 M instructions (1.60 M a guest second, 6,534 a pass), all cores 95.1 M, 256 cores ran. Core 0 took 453,476 write-cache entries (155 distinct texels a pass that stored), and 120 of its passes ended full (5.5 a guest second); the workers' 25 (1.1 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 4.1 | 2.1 | 33.1 |
| libc copy/fill/str | 4.3 | 1.8 | 0 |
| libc other | 10.5 | 7.7 | 0.0 |
| soft float, 64-bit | 0.3 | 0.1 | 0.3 |
| kernel | 68.7 | 80.4 | 0 |
| window server | 8.8 | 4.7 | 2.7 |
| window client | 0.4 | 0.6 | 0 |
| unknown | 2.9 | 2.7 | 63.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 9.4 | kernel:__free_one_page | 5.9 |
| kernel:alloc_pages_run | 5.2 | kernel:handle_exception | 4.5 |
| kernel:__free_one_page | 5.0 | kernel:free_unref_page_commit.isra.0 | 3.8 |
| kernel:load_flat_binary | 4.6 | kernel:alloc_pages_run | 3.6 |
| nano-X:memset | 2.7 | kernel:free_page_series | 3.4 |
| kernel:free_unref_page | 2.1 | kernel:__memcpy | 3.1 |
| kernel:free_unref_page_commit.isra.0 | 1.7 | kernel:__memset | 2.6 |
| nxmon:vfscanf | 1.7 | kernel:load_flat_binary | 2.2 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 42, nxmon:claim.isra.0 27, kernel:__memcpy 7, kernel:handle_exception 5, kernel:__free_one_page 3.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 30.4 | nxmon:shade_plot | 35.9 |
| nxmon:shade_plot | 30.1 | ?0x81712000 | 28.0 |
| ?0x81711000 | 24.3 | ?0x81711000 | 18.3 |
| ?0x81715000 | 8.5 | ?0x81715000 | 8.3 |
| nxmon:shade_region | 3.0 | nano-X:memset | 5.4 |
| nano-X:memset | 2.4 | nxmon:shade_region | 1.5 |

The workers' passes ended full, where (passes): nxmon:shade_region 25.

Slow forms prof.py finds (% of core 0): 2.5%  [printf-in-hot-path] nxmon:vfprintf  (929 calls of 956); 2.0%  [printf-in-hot-path] nxmon:printf_core  (1,858 calls of 387).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 4.84% of core 0's instructions and 2.41% of its write-cache entries, calls of about 2 KB or more 1.76% (nano-X:memset >=2KB 5 calls of 124,150 instructions; nxmon:memmove 2,358 calls of 142 instructions; nano-X:memset 3,184 calls of 97 instructions). The kernel's `__memcpy` 1.62% (byte paths 0.66%, 64-byte word loop 0.33%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.71.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,421 | 652,421 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |
| nxbar:build | 1 | 451,034 | 451,034 |
| nxbar:qsort | 2 | 378,813 | 757,290 |

### network

Network: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 u 300 enter 300 n 300 enter; sleep 8; ps | grep -v grep | grep "nxnet" | sed "s/^/R`...

Window: `the command` to `APPDONE`: 21.7 s of the guest, 4,680 passes; core 0 31.6 M instructions (1.46 M a guest second, 6,759 a pass), all cores 71.5 M, 256 cores ran. Core 0 took 413,524 write-cache entries (156 distinct texels a pass that stored), and 117 of its passes ended full (5.4 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 3.5 | 1.7 | 0 |
| libc copy/fill/str | 3.3 | 1.5 | 0 |
| libc other | 9.6 | 7.5 | 0.0 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 71.2 | 81.1 | 0 |
| window server | 8.2 | 4.3 | 4.1 |
| window client | 0.3 | 0.5 | 0 |
| unknown | 3.9 | 3.4 | 95.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 10.4 | kernel:__free_one_page | 6.5 |
| kernel:alloc_pages_run | 5.6 | kernel:handle_exception | 4.4 |
| kernel:__free_one_page | 5.6 | kernel:free_unref_page_commit.isra.0 | 4.2 |
| kernel:load_flat_binary | 5.1 | kernel:alloc_pages_run | 3.9 |
| nano-X:memset | 3.0 | kernel:free_page_series | 3.8 |
| kernel:free_unref_page | 2.4 | kernel:__memcpy | 2.8 |
| kernel:free_unref_page_commit.isra.0 | 1.9 | kernel:__memset | 2.6 |
| nxnet:vfscanf | 1.8 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 44, ?0x81724000 27, kernel:handle_exception 5, kernel:__memcpy 5, kernel:__memset 4.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.4 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nano-X:memset | 3.6 | nano-X:memset | 8.6 |
| nano-X:memchr | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.83% of core 0's instructions and 2.09% of its write-cache entries, calls of about 2 KB or more 1.96% (nano-X:memset >=2KB 5 calls of 124,150 instructions; nano-X:memset 3,224 calls of 97 instructions; nxbar:memcpy 7,296 calls of 25 instructions). The kernel's `__memcpy` 1.51% (byte paths 0.65%, 64-byte word loop 0.3%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.9.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,421 | 652,421 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |
| nxbar:build | 1 | 451,034 | 451,034 |
| nxbar:qsort | 2 | 378,813 | 757,290 |

### settings

Settings: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 u 300 enter 300 s 300 enter; sleep 8; ps | grep -v grep | grep "nxsettings" | sed "`...

Window: `the command` to `APPDONE`: 21.7 s of the guest, 4,788 passes; core 0 32.7 M instructions (1.50 M a guest second, 6,835 a pass), all cores 72.6 M, 256 cores ran. Core 0 took 420,943 write-cache entries (154 distinct texels a pass that stored), and 111 of its passes ended full (5.1 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 4.1 | 2.2 | 95.4 |
| libc copy/fill/str | 4.0 | 2.2 | 0 |
| libc other | 7.5 | 4.5 | 0 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 70.2 | 80.6 | 0 |
| window server | 5.8 | 2.8 | 0 |
| window client | 0.2 | 0.3 | 0 |
| unknown | 8.3 | 7.3 | 4.6 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 10.1 | kernel:__free_one_page | 6.3 |
| kernel:alloc_pages_run | 5.5 | kernel:handle_exception | 4.4 |
| kernel:__free_one_page | 5.4 | kernel:free_unref_page_commit.isra.0 | 4.1 |
| kernel:load_flat_binary | 5.0 | kernel:alloc_pages_run | 3.9 |
| kernel:free_unref_page | 2.3 | kernel:free_page_series | 3.7 |
| kernel:free_unref_page_commit.isra.0 | 1.9 | kernel:__memcpy | 2.9 |
| nano-X:memset | 1.8 | kernel:__memset | 2.6 |
| kernel:__memcpy | 1.8 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 41, ?0x81724000 28, kernel:handle_exception 5, kernel:__memcpy 5, kernel:__free_one_page 5.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| nxsettings:mcu_pixels | 45.9 | nxsettings:mcu_pixels | 45.6 |
| nxsettings:idct.constprop.0 | 36.3 | nxsettings:idct.constprop.0 | 28.4 |
| nxsettings:restart_intervals | 13.1 | nxsettings:restart_intervals | 14.8 |
| ?0x81736000 | 4.1 | ?0x81736000 | 8.6 |
| ?0x81726000 | 0.4 | ?0x81726000 | 1.6 |
| nxsettings:markers_part | 0.1 | nxsettings:markers_part | 0.5 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 10,364 (0.03% of core 0, 295 places).

Slow forms prof.py finds (% of core 0): 1.8%  [large-clear] nano-X:memset  (28 calls of 21,565).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.38% of core 0's instructions and 2.13% of its write-cache entries, calls of about 2 KB or more 1.84% (nano-X:memset >=2KB 2 calls of 300,690 instructions; nxbar:memcpy 7,304 calls of 25 instructions; nxsettings#2:memcpy 5,768 calls of 23 instructions). The kernel's `__memcpy` 1.8% (byte paths 0.82%, 64-byte word loop 0.29%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.84.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxsettings:ui_text.constprop.0 | 1 | 834,297 | 834,297 |
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |
| nxsettings:media_image_decode_fit | 1 | 650,503 | 650,503 |
| nxsettings:media_image_decode | 1 | 639,514 | 639,514 |

### eyes

Eyes: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 o 300 enter 300 e 300 enter; sleep 8; ps | grep -v grep | grep "nxeyes" | sed "s/^/`...

Window: `the command` to `APPDONE`: 21.8 s of the guest, 4,755 passes; core 0 32.0 M instructions (1.47 M a guest second, 6,736 a pass), all cores 71.9 M, 256 cores ran. Core 0 took 416,407 write-cache entries (158 distinct texels a pass that stored), and 104 of its passes ended full (4.8 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 3.5 | 1.8 | 0 |
| libc copy/fill/str | 3.9 | 2.1 | 3.7 |
| libc other | 6.9 | 6.6 | 0.0 |
| soft float, 64-bit | 0.1 | 0.1 | 0 |
| kernel | 70.7 | 81.1 | 0 |
| window server | 9.6 | 3.7 | 0 |
| window client | 0.2 | 0.3 | 0 |
| unknown | 5.1 | 4.4 | 96.3 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 10.3 | kernel:__free_one_page | 6.4 |
| kernel:alloc_pages_run | 5.6 | kernel:handle_exception | 4.4 |
| kernel:__free_one_page | 5.5 | kernel:free_unref_page_commit.isra.0 | 4.1 |
| kernel:load_flat_binary | 5.1 | kernel:alloc_pages_run | 3.9 |
| kernel:free_unref_page | 2.3 | kernel:free_page_series | 3.7 |
| kernel:free_unref_page_commit.isra.0 | 1.9 | kernel:__memcpy | 2.7 |
| nano-X:memset | 1.9 | kernel:__memset | 2.5 |
| nano-X:vfscanf | 1.7 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 41, ?0x81724000 28, kernel:handle_exception 6, kernel:__free_one_page 4, kernel:__memcpy 4.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nxeyes:memset | 3.7 | nxeyes:memset | 8.6 |
| ?0x81736000 | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 30,941 (0.1% of core 0, 290 places).

Slow forms prof.py finds (% of core 0): 1.9%  [large-clear] nano-X:memset  (23 calls of 26,232).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.71% of core 0's instructions and 1.91% of its write-cache entries, calls of about 2 KB or more 1.94% (nano-X:memset >=2KB 2 calls of 300,690 instructions; nxeyes:memset 3,309 calls of 97 instructions; nxbar:memcpy 7,312 calls of 25 instructions). The kernel's `__memcpy` 1.48% (byte paths 0.63%, 64-byte word loop 0.28%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.88.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |

### glxgears

Gears (OpenGL): started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 o 300 enter 300 g 300 enter; sleep 8; ps | grep -v grep | grep "glxgears" | sed "s/`...

Window: `the command` to `APPDONE`: 22.6 s of the guest, 7,761 passes; core 0 59.7 M instructions (2.64 M a guest second, 7,696 a pass), all cores 99.6 M, 256 cores ran. Core 0 took 630,792 write-cache entries (146 distinct texels a pass that stored), and 142 of its passes ended full (6.3 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 1.9 | 1.2 | 0.0 |
| libc copy/fill/str | 0.9 | 0.4 | 3.7 |
| libc other | 3.4 | 4.1 | 0.0 |
| libm | 0.1 | 0.1 | 0 |
| soft float, 64-bit | 3.0 | 0.4 | 0 |
| kernel | 38.3 | 55.0 | 0 |
| window server | 6.0 | 4.6 | 0 |
| window client | 0.1 | 0.2 | 0 |
| unknown | 46.4 | 34.1 | 96.3 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| ?0x818d1000 | 22.0 | ?0x818d1000 | 22.9 |
| ?0x818d2000 | 11.2 | ?0x818d4000 | 4.6 |
| kernel:free_pcppages_bulk | 5.5 | kernel:__free_one_page | 4.2 |
| ?0x818d0000 | 5.1 | kernel:handle_exception | 3.2 |
| kernel:alloc_pages_run | 3.0 | kernel:free_unref_page_commit.isra.0 | 2.7 |
| kernel:__free_one_page | 3.0 | kernel:alloc_pages_run | 2.6 |
| kernel:load_flat_binary | 2.7 | kernel:free_page_series | 2.5 |
| ?0x818d3000 | 1.8 | nano-X:memcpy | 2.0 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 42, ?0x81724000 28, nano-X:memcpy 27, kernel:handle_exception 8, kernel:__memcpy 6.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| glxgears:memset | 3.7 | glxgears:memset | 8.6 |
| ?0x81736000 | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

Slow forms prof.py finds (% of core 0): 7.3%  [independent-pixel-loop] ?0x818d2000  (150,908 trips of 29); 1.9%  [independent-pixel-loop] ?0x818d1000  (100,604 trips of 11); 1.2%  [software-float] glxgears:__muldf3  (7,402 calls of 100); 1.1%  [software-float] glxgears:__subdf3  (3,831 calls of 167).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.33% of core 0's instructions and 2.95% of its write-cache entries, calls of about 2 KB or more 1.04% (nano-X:memset >=2KB 2 calls of 300,690 instructions; nano-X:memset 3,731 calls of 89 instructions; nano-X:memcpy 10,014 calls of 29 instructions). The kernel's `__memcpy` 0.81% (byte paths 0.35%, 64-byte word loop 0.16%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): glxgears:__cos 0.85, ?0x818cf000 0.77, glxgears:__sin 0.69, glxgears:__rem_pio2 0.43, ?0x818dd000 0.15.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.01.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,421 | 652,421 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |

### imgui

Dear ImGui: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 o 300 enter 300 d 300 enter; sleep 8; ps | grep -v grep | grep "imdemo" | sed "s/^/`...

Window: `the command` to `APPDONE`: 22.6 s of the guest, 7,808 passes; core 0 59.8 M instructions (2.64 M a guest second, 7,655 a pass), all cores 112.1 M, 256 cores ran. Core 0 took 714,468 write-cache entries (191 distinct texels a pass that stored), and 186 of its passes ended full (8.2 a guest second); the workers' 513 (22.7 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 33.9 | 29.2 | 23.3 |
| libc copy/fill/str | 1.8 | 1.3 | 2.8 |
| libc other | 4.4 | 3.1 | 0 |
| kernel | 38.5 | 49.6 | 0 |
| window server | 3.8 | 1.8 | 0.3 |
| window client | 0.1 | 0.2 | 0 |
| unknown | 17.3 | 14.8 | 73.6 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| imdemo:_ZN10ImDrawList11AddPolylineEPK6ImVec2ijif.part.0 | 9.8 | imdemo:_ZN10ImDrawList11AddPolylineEPK6ImVec2ijif.part.0 | 11.1 |
| imdemo:_ZN6ImFont10RenderTextEP10ImDrawListfRK6ImVec2jRK6ImVec4PKcS9_fb.isra.0 | 9.6 | imdemo:_ZN6ImFont10RenderTextEP10ImDrawListfRK6ImVec2jRK6ImVec4PKcS9_fb.isra.0 | 5.7 |
| kernel:free_pcppages_bulk | 5.4 | kernel:__free_one_page | 3.8 |
| ?0x81a6f000 | 3.8 | kernel:handle_exception | 3.1 |
| kernel:alloc_pages_run | 3.1 | kernel:free_unref_page_commit.isra.0 | 2.4 |
| kernel:__free_one_page | 3.0 | kernel:alloc_pages_run | 2.4 |
| kernel:load_flat_binary | 2.8 | kernel:free_page_series | 2.2 |
| imdemo:_ZN10ImDrawList11PrimReserveEii | 2.4 | imdemo:_ZN10ImDrawList11PrimReserveEii | 2.1 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 46, ?0x81724000 28, ?0x81bbe000 27, imdemo:_Z24ImGui_ImplShaderEmu_Initj 26, kernel:handle_exception 7.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 34.8 | ?0x81712000 | 30.3 |
| ?0x81711000 | 27.8 | imdemo:_ZN6copier12copy_commandERK6ImVec4PKcPKtS6_ | 25.0 |
| imdemo:_ZN6copier12copy_commandERK6ImVec4PKcPKtS6_ | 20.7 | ?0x81711000 | 19.8 |
| ?0x81715000 | 9.7 | ?0x81715000 | 9.0 |
| imdemo:memset | 2.8 | imdemo:memset | 5.8 |
| imdemo:_ZN6copier8colouredEPKtPKjS3_S3_S3_ | 1.7 | imdemo:_ZN6copier8colouredEPKtPKjS3_S3_S3_ | 3.6 |

The workers' passes ended full, where (passes): imdemo:_ZN6copier12copy_commandERK6ImVec4PKcPKtS6_ 482, imdemo:_ZN6copier8colouredEPKtPKjS3_S3_S3_ 30, imdemo:_ZL8copy_jobjj.lto_priv.0 1.

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 8,763 (0.01% of core 0, 294 places).

Slow forms prof.py finds (% of core 0): 1.6%  [division-in-loop] imdemo:_ZN10ImDrawList11AddPolylineEPK6ImVec2ijif.part.0+0x9c  (17,260 trips of 56).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.02% of core 0's instructions and 1.19% of its write-cache entries, calls of about 2 KB or more 1.04% (nano-X:memset >=2KB 2 calls of 300,690 instructions; imdemo:memset 3,049 calls of 98 instructions; nxbar:memcpy 7,257 calls of 25 instructions). The kernel's `__memcpy` 0.82% (byte paths 0.36%, 64-byte word loop 0.16%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.01.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nxbar:select | 23 | 960,584 | 3,220,422 |
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |

### music

the Music player: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 o 300 enter 300 m 300 enter; sleep 8; ps | grep -v grep | grep "nxplay" | sed "s/^/`...

Window: `the command` to `APPDONE`: 22.6 s of the guest, 7,786 passes; core 0 59.8 M instructions (2.64 M a guest second, 7,677 a pass), all cores 99.6 M, 256 cores ran. Core 0 took 520,344 write-cache entries (105 distinct texels a pass that stored), and 117 of its passes ended full (5.2 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 11.0 | 7.1 | 0.0 |
| libc copy/fill/str | 2.8 | 4.1 | 3.7 |
| libc other | 5.9 | 6.7 | 0.0 |
| libm | 2.1 | 1.8 | 0 |
| soft float, 64-bit | 30.8 | 3.2 | 0 |
| kernel | 39.0 | 69.4 | 0 |
| window server | 5.5 | 3.6 | 0 |
| window client | 0.1 | 0.4 | 0 |
| unknown | 2.8 | 3.6 | 96.3 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| nxplay:__adddf3 | 15.8 | kernel:__free_one_page | 5.2 |
| nxplay:__muldf3 | 12.8 | nxplay:stb_vorbis_open_memory | 4.5 |
| kernel:free_pcppages_bulk | 5.5 | kernel:handle_exception | 4.1 |
| nxplay:stb_vorbis_open_memory | 4.7 | kernel:free_unref_page_commit.isra.0 | 3.3 |
| kernel:alloc_pages_run | 3.0 | kernel:alloc_pages_run | 3.2 |
| kernel:__free_one_page | 3.0 | kernel:free_page_series | 3.0 |
| kernel:load_flat_binary | 2.7 | kernel:__memcpy | 2.6 |
| nxplay:get_bits | 1.6 | nxplay:memset | 2.5 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 46, ?0x81724000 28, kernel:__memcpy 8, kernel:handle_exception 5, kernel:__free_one_page 4.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nxbar:memset | 3.7 | nxbar:memset | 8.6 |
| ?0x81736000 | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 17,039 (0.03% of core 0, 294 places).

Slow forms prof.py finds (% of core 0): 12.8%  [software-float] nxplay:__muldf3  (76,347 calls of 100); 10.9%  [software-float] nxplay:__subdf3  (40,328 calls of 161); 6.6%  [software-float] nxplay:__adddf3  (28,621 calls of 138).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 2.65% of core 0's instructions and 4.35% of its write-cache entries, calls of about 2 KB or more 1.05% (nano-X:memset >=2KB 2 calls of 300,690 instructions; nxplay:memset 2,197 calls of 143 instructions; nxplay:memcpy 11,199 calls of 24 instructions). The kernel's `__memcpy` 0.85% (byte paths 0.35%, 64-byte word loop 0.19%).
 Soft floats and 64-bit divides, inclusive, by caller (% of core 0): nxplay:__cos 15.59, nxplay:__sin 6.93, nxplay:sin 4.07, nxplay:__rem_pio2 2.29, nxplay:pow 1.0, nxplay:init_blocksize 0.46.
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.01.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |

### nes-nova

Nova the Squirrel (NES): started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 g 300 enter 300 n 300 enter; sleep 8; ps | grep -v grep | grep "nes nova" | sed "s/`...

Window: `the command` to `APPDONE`: 22.6 s of the guest, 21,075 passes; core 0 42.2 M instructions (1.87 M a guest second, 2,005 a pass), all cores 90.4 M, 256 cores ran. Core 0 took 583,062 write-cache entries (52 distinct texels a pass that stored), and 149 of its passes ended full (6.6 a guest second); the workers' 28 (1.2 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 5.4 | 2.8 | 10.3 |
| libc copy/fill/str | 2.1 | 2.6 | 3.5 |
| libc other | 7.3 | 5.0 | 0.0 |
| soft float, 64-bit | 0.2 | 0.0 | 0.1 |
| kernel | 69.4 | 77.2 | 0 |
| window server | 6.2 | 2.7 | 0 |
| window client | 0.1 | 0.2 | 0 |
| unknown | 9.3 | 9.5 | 86.2 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 7.7 | kernel:handle_exception | 10.7 |
| kernel:handle_exception | 6.1 | kernel:__free_one_page | 4.7 |
| kernel:alloc_pages_run | 5.1 | ?0x81aad000 | 3.1 |
| kernel:__free_one_page | 4.3 | kernel:alloc_pages_run | 3.1 |
| kernel:load_flat_binary | 4.1 | kernel:free_unref_page_commit.isra.0 | 3.0 |
| ?0x81aad000 | 2.6 | kernel:__fget_light | 3.0 |
| kernel:sys_ioctl | 2.5 | ?0x81aae000 | 2.9 |
| nano-X:memset | 2.2 | kernel:single_task_running | 2.8 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 45, ?0x81724000 28, nes:memset 25, kernel:handle_exception 5, kernel:__memcpy 5.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 37.9 | ?0x81712000 | 35.1 |
| ?0x81711000 | 30.2 | ?0x81711000 | 22.9 |
| ?0x81715000 | 10.6 | ?0x81715000 | 10.5 |
| nxbar:memset | 3.0 | nxbar:memset | 6.7 |
| nes:nes_renderframe.constprop.0 | 2.0 | ?0x81aad000 | 5.3 |
| ?0x81aad000 | 1.5 | nes:nes_renderframe.constprop.0 | 1.6 |

The workers' passes ended full, where (passes): nes:gd_tile 13, nes:seglSprite 13, nes:nes_renderframe.constprop.0 1, nes:rc_line 1.

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.38% of core 0's instructions and 3.16% of its write-cache entries, calls of about 2 KB or more 1.8% (nano-X:memset >=2KB 2 calls of 300,690 instructions; nano-X:memset 3,343 calls of 91 instructions; nxbar#2:memcpy 7,345 calls of 25 instructions). The kernel's `__memcpy` 1.19% (byte paths 0.53%, 64-byte word loop 0.23%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.42, ?0x81a6e000 11 0.31.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,421 | 652,421 |

### example-cube

the IDE with the Cube example: started from the Start menu, 8 s. `killall nano-X; sleep 1; export UI_LAUNCH=print; nx; sleep 4; nxkey meta 400 p 300 enter 300 c 300 enter; sleep 8; ps | grep -v grep | grep "example-cube" | sed`...

Window: `the command` to `APPDONE`: 21.9 s of the guest, 4,791 passes; core 0 32.7 M instructions (1.49 M a guest second, 6,821 a pass), all cores 72.6 M, 256 cores ran. Core 0 took 425,533 write-cache entries (155 distinct texels a pass that stored), and 121 of its passes ended full (5.5 a guest second); the workers' 0 (0.0 a guest second).

| kind of code | core 0 instructions (%) | core 0 write-cache entries (%) | workers' instructions (%) |
|---|---|---|---|
| program | 4.0 | 1.8 | 0 |
| libc copy/fill/str | 4.3 | 2.4 | 3.7 |
| libc other | 10.1 | 7.8 | 0.5 |
| soft float, 64-bit | 0.1 | 0.0 | 0 |
| kernel | 70.7 | 80.8 | 0 |
| window server | 6.6 | 3.3 | 0 |
| window client | 0.3 | 0.5 | 0 |
| unknown | 3.8 | 3.5 | 95.9 |

| core 0: function | instructions (% of core 0) | core 0: function | write-cache entries (% of core 0's) |
|---|---|---|---|
| kernel:free_pcppages_bulk | 10.1 | kernel:__free_one_page | 6.3 |
| kernel:alloc_pages_run | 5.5 | kernel:handle_exception | 4.3 |
| kernel:__free_one_page | 5.4 | kernel:free_unref_page_commit.isra.0 | 4.1 |
| kernel:load_flat_binary | 5.0 | kernel:alloc_pages_run | 3.9 |
| kernel:free_unref_page | 2.3 | kernel:free_page_series | 3.7 |
| kernel:__memcpy | 1.9 | kernel:__memcpy | 3.2 |
| kernel:free_unref_page_commit.isra.0 | 1.9 | kernel:__memset | 2.7 |
| nano-X:memset | 1.8 | kernel:load_flat_binary | 2.4 |

Core 0's passes ended full, where (passes): kernel:alloc_pages_run 43, ?0x81724000 28, kernel:__memcpy 12, kernel:handle_exception 6, kernel:__free_one_page 4.

| workers: function | instructions (% of the workers') | workers: function | write-cache entries (% of the workers') |
|---|---|---|---|
| ?0x81712000 | 45.7 | ?0x81712000 | 44.8 |
| ?0x81711000 | 36.5 | ?0x81711000 | 29.2 |
| ?0x81715000 | 12.8 | ?0x81715000 | 13.3 |
| nxide:memset | 3.7 | nxide:memset | 8.6 |
| tcc:memchr | 0.4 | ?0x81726000 | 1.6 |
| ?0x81726000 | 0.4 | ?0x81716000 | 0.9 |

GOT loads executed on core 0 (`auipc`+`lw` of a `.got` word): nano-X 12,822 (0.04% of core 0, 307 places).

Slow forms prof.py finds (% of core 0): 1.8%  [large-clear] nano-X:memset  (28 calls of 21,565).

Copies and fills (prof_forms.py): the programs' memcpy, memmove and memset 3.78% of core 0's instructions and 2.15% of its write-cache entries, calls of about 2 KB or more 1.9% (nano-X:memset >=2KB 2 calls of 300,690 instructions; tcc:memset 2,580 calls of 101 instructions; nxbar:memcpy 7,325 calls of 25 instructions). The kernel's `__memcpy` 1.86% (byte paths 0.84%, 64-byte word loop 0.32%).
 Fills of 2,000 instructions or more a call, by caller (calls, % of core 0): nano-X:gpu_open 2 1.84.

| a job for a worker? (RVC_CALLS, core 0) | calls | instructions a call | the longest |
|---|---|---|---|
| nano-X:GsInitialize | 1 | 652,422 | 652,422 |
| nano-X:GdOpenScreen | 1 | 609,382 | 609,382 |
| nano-X:gpu_open | 1 | 607,654 | 607,654 |

