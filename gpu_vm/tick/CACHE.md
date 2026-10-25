# The write cache of today's tick (c8_v0001, 25 October 2026)

Today's tick is `build/tick_gen/c8/v0001.hlsl`: builder point c4_v0001 with idlew 4, made by
`gpu_vm/tick/builder/gen.py`. `gpu_vm/tick/readable/tick.hlsl` is the same machine with the builder's unused switches
taken out, and its line numbers are cited below. The point's cache switches are nb 32, hash 1, bloom 1, evf 4,
evmax 8, c0nb 48 and xr 0 (no core file).

## Why there is a cache at all

A pixel shader may write only its own pixels' render targets, and RAM is a texture the tick can only read. A core's
stores during a pass therefore cannot reach RAM. They are kept in the core's state as a write cache, and after the
tick a separate draw, the commit, writes them into RAM. The cache also has to answer a core's own loads, because a
core must see its own stores within the pass.

## What an entry is

- **An entry is one RAM texel** (16 bytes, four words), plus a tag.
- **The tag** is the texel's number in bits 0-27 and, in bits 28-31, which of its four words were stored. Empty is
  `0xffffffff`.
- **The size.** Two tables of 32 buckets of four ways make 256 entries, plus two spare entries: 258 for a worker.
  Core 0 has 48 buckets a table, 386 entries.
- **Hashes.** A texel can live in one bucket of each table, a two-choice table (lines 167-168):
  `first = ((t * 0x9e3779b1 >> 16) * 32) >> 16` and `second = 32 + ((t * 0x85ebca6b + 0x165667b1 >> 16) * 32) >> 16`.
- **Where it is kept.** The tags and the words are local arrays of the shader (`cacheTags[]`, `cacheWords[]`), so on
  NVIDIA they are in local memory behind the L1. Every lane of the core keeps its own full copy: up to 46 copies a core.
- **Extra state.** `storedTexels` is the OR of every texel number stored in the pass, a one-word bloom filter.

## A pass

1. **The start** (`load_state`, line 690): every tag is set empty (a loop over all entries) and `storedTexels = 0`.
   Nothing is kept between passes: last pass's entries are in RAM by now.
2. **A store** (`fast_step` paths 5 and 6, and the general step's `store_any`):
   1. Read the four tags of each of the texel's two buckets.
   2. Take the way that already holds the texel; if none does, take the first empty way, the first table before the
      second (`find_slot`).
   3. Merge the stored bytes into the entry's word and set the word's bit in the tag. An aligned word store reads no
      RAM; a byte or half store, or an unaligned one, reads the RAM texel to merge into.
   4. OR the texel into `storedTexels`.
   5. If neither bucket has room, the store goes to a spare entry and the pass ends after this bundle (a "full end").
3. **A load:**
   1. If the texel's bits are not all in `storedTexels`, the core cannot have stored it this pass, so the tags are not
      read (bloom 1).
   2. Otherwise the two buckets are looked up as for a store.
   3. The RAM texel is always fetched. The word comes from the entry if the entry holds that word, else from RAM.
4. **A full cache** (`make_room`, line 635; evf 4): instead of ending the pass, the tick reads the RAM texel of every
   entry. It drops the entries whose words equal RAM's (silent stores: since RAM does not change during a pass, an
   entry equals RAM only when the store changed nothing) and moves the spare entries into the freed ways. The pass
   then goes on with its budget left, up to 8 times (evmax 8). If nothing could be dropped, the pass ends full.
5. **The end:** the entries are written out as state texels. The words come at T_WC = 40, one texel an entry, and the
   tags at T_WT = T_WC + 258, four a texel.

## The commit

The commit is four draws of points, one for each component x, y, z and w (`commit_vert`, line 790), with a write mask
of that component alone:

- There is one point for each entry of every core: core 0's first, the highest core's last.
- A point whose tag is empty, or whose tag lacks this word's bit, is placed off screen.
- If two cores store one word in the same pass, the higher core's value wins (the order of the draw).

## What a core sees

- its own stores of this pass, through the cache;
- every core's stores from earlier passes, in RAM;
- **not** another core's stores of this pass. Cores talk to each other at the granularity of a pass.

## What it costs, measured

| measured | value | source |
|---|---|---|
| a load's texel alone, one core | 83.4 ns a bundle | IDEAS 29, probes/floor |
| the same with x23's two-bucket lookup | 110.7 ns a bundle | IDEAS 29 |
| a direct-mapped store | 422 ns a bundle | IDEAS 29 |
| local memory in Quake III's workers | 93% of L1 sectors; loads hit 95.3%, stores 59.3% (written through to L2) | IDEAS 34 |
| every probe at 255 busy cores | 1.2-4x dearer than one core (each lane keeps its own copy) | IDEAS 29 |
| the write cache's size against Doom's full-cache passes | 1,840 full ends to 93 with a second bucket to try | AGENTS.md |
| silent stores (sil 1-3) | Quake III's workers 60-67% of stores silent, passes halved, fps as before; gvmbench 5-10% slower: not taken | IDEAS 33, 39 |
| the boot, by cache size (gvm_cpu, 4,096 bundles a pass) | 4,589 passes with v0009's cache, 1,909 with 258 entries, 1,589 with 514, 1,553 with 1,026 | commit 45c71f7 |

## Where the cost comes from

- **Every lane does all the work.** A core's 46 lanes each look up, merge and keep the whole cache. That is 46 copies
  in local memory, and 46 times the L1 traffic of one lane.
- **Every pass clears every tag,** which is 258 local stores before the first bundle.
- **Every load reads RAM** even when the cache holds its word, because a texel's words can be partly cached.
- **A full cache ends the pass** (an early end costs 0.2 ms in the harness, AGENTS.md), or else `make_room` reads all
  258 entries' RAM texels.
- **The cache's size sets the block's size** (363 texels, 46 pixels), and so how cores fall into warps (WAVES.md).

## Decided for the new tick (25 October 2026)

- **The commit is one draw, a point a texel, the whole texel.**
  - The core knows the texel it changed: RAM's texel as the pass found it, with its own stores merged in. So the
    commit writes that whole texel. The words it did not store are written back with the values they already had.
  - No per-channel draws, no write masks, no blending. (Blending is not allowed on `R32G32B32A32_UINT` targets anyway,
    and D3D11.1's logic ops are not in Unity.)
  - The cost moves into the tick: an entry needs RAM's texel. Either it is fetched when the entry is made, or, at the
    pass's end, one loop of independent fetches fills the entries that did not get all four words (75% get all four,
    `memory_model.md`).
  - What it asks of programs: two cores must not store into one texel in the same pass, or the later core's whole texel
    wins. The RISC-V machine already worked this way (gles.c's worker-written statics are on 16-byte sections because
    of it), and `RVC_RACES` checks it there. The reference must check the same.
- **One array, probed in turn.**
  - Today's "two tables" are already one array with two places a texel may go. But every lookup reads both buckets,
    8 tags, even when the first bucket has an empty way.
  - The new cache probes bucket after bucket (the next bucket, or a rehash) and stops at the first empty way, as open
    addressing does.
  - Entries are only added during a pass and the whole cache is cleared at its start, so stopping at an empty way is
    exact. Most lookups would read one bucket. Only dropping entries at a full cache (evf) would make holes, and those
    need tombstones or a repack.
- **The "did I store this?" test: a hashed bitmap instead of the OR word.**
  - Today `storedTexels` is the OR of every texel number stored in the pass, and a load skips the lookup when its
    texel's bits are not all in it. After a few stores at scattered addresses nearly every bit is set, so nearly every
    load goes on to the lookup.
  - A bitmap of a few hundred bits, one bit a hash of the texel, keeps working as stores accumulate. With 256 bits and
    50 stored texels, about 18% of clean loads would still look up.
  - The bitmap can only say "not stored". A load of a stored texel still needs the entry, for its value.
  - With whole-texel commits the tag needs no per-word bits either. It is the texel's number alone.
- **A hit reads no RAM.**
  - Today a load fetches RAM's texel even when the cache holds its word, since an entry may hold only some words.
  - A whole-texel entry has all four, so a hit is answered from local memory alone (about 15 ns, against a texel
    fetch of 35 ns warm and about 65 ns as the games see it, IDEAS 29, 30, 34).
  - 28-56% of loads read a texel the core stored earlier in the pass (the floor in "Loads that must look up").
- **No read cache, and no cache carried across passes.**
  - Of the loads RAM must answer, 24-68% touch a texel the same core touched in the last pass, but only 1-15% one it
    stored ("wcstudy" section E, 25 October 2026).
  - Reloading a carried cache at every pass's start is cold reads again. IDEAS 7 measured a 256-texel file nothing
    used at 12-16%, and IDEAS 40 found warming 16 KB a core at a pass's start dearer than it saved, against roughly 3%
    to gain.
  - It would also need an exact check against other cores' stores, and a second cache in the tick.
  - Without compute shaders there is no group-shared memory to put one in. Not taken.

## Measured on the reference (25 October 2026)

`gpu_vm/cpu/wcstudy.c` replays a gvm_cpu log of every core's loads and stores (`GVM_STLOG_CORE=-1 GVM_STLOG_LOADS=1`,
`--wc 32,4`). The logs:

- Quake III: `build/profile_q3/gvm_cpu/q3ld.stlog`, 100 frames of q3dm17 with 3 bots, img-q3fix;
- Doom's timedemo, Quake's timedemo, and wl's every-worker mode (its first kernels): 20 million records each, on
  img-cur and build/wl/img, 32 cores.

Core 0's passes in these logs end where today's cache was full, so the full ends below compare designs on core 0 and
are not absolute counts. The workers' passes are not cut by any cache, so their counts are real.

**How much of an entry is used** (texels stored in a pass):

| run | core | mean words used, of 4 | texels with 4 words, % | texels with 1 word, % | stores of 4 bytes, % |
|---|---|---|---|---|---|
| Quake III | 0 | 3.14 | 64.5 | 20.4 | 97.8 |
| Quake III | workers | 2.47 | 47.7 | 49.4 | 98.9 |
| Doom | 0 | 2.85 | 53.6 | 28.8 | 88.2 |
| Doom | workers | 2.16 | 32.3 | 53.5 | 94.9 |
| Quake | 0 | 3.23 | 67.2 | 19.1 | 97.3 |
| Quake | workers | 3.37 | 75.0 | 16.6 | 96.1 |
| wl, every worker | 0 | 3.27 | 68.5 | 13.7 | 94.8 |
| wl, every worker | workers | 2.58 | 48.1 | 40.3 | 100.0 |

No store is 8 or 16 bytes. A texel entry costs 5 words (a tag and four values); word entries for the same stores
would cost 2 x the mean words. They come out smaller only on Doom's workers (4.3 against 5) and Quake III's (4.9). Word
or byte entries are not worth their extra lookups and a commit that would have to regroup words into texels.

**Loads that must look up** (% of loads):

| run | core | stored this pass (the floor) | today's OR word | 256-bit bitmap | 1,024-bit bitmap |
|---|---|---|---|---|---|
| Quake III | 0 | 53.0 | 78.9 | 62.9 | 55.8 |
| Quake III | workers | 39.7 | 64.5 | 53.2 | 43.3 |
| Doom | 0 | 38.7 | 81.0 | 55.5 | 42.8 |
| Doom | workers | 27.9 | 67.0 | 42.1 | 32.4 |
| Quake | 0 | 40.0 | 84.2 | 58.8 | 45.2 |
| Quake | workers | 30.9 | 71.6 | 55.8 | 38.5 |
| wl, every worker | 0 | 7.3 | 13.7 | 9.7 | 7.8 |
| wl, every worker | workers | 55.9 | 85.1 | 66.5 | 58.8 |

Two hashes into 256 bits are no better than one.

**Buckets read, and full ends** (one array of 64 buckets of 4, probed bucket by bucket, stopping at the first empty
way, at most 4 buckets; against today's two buckets always read):

| run | core | today: buckets a store, a load | probed: buckets a store | probed: buckets a load | today: passes full, % of passes that store | probed: passes full, % |
|---|---|---|---|---|---|---|
| Quake III | 0 | 2, 2 | 1.07 | 1.10 | 25.8 | 8.6 |
| Quake III | workers | 2, 2 | 1.29 | 1.53 | 0.12 | 0.11 |
| Doom | 0 | 2, 2 | 1.03 | 1.08 | 14.2 | 5.7 |
| Doom | workers | 2, 2 | 1.28 | 1.49 | 0.19 | 0.18 |
| Quake | 0 | 2, 2 | 1.06 | 1.12 | 57.6 | 24.6 |
| Quake | workers | 2, 2 | 1.77 | 1.88 | 1.01 | 0.96 |
| wl, every worker | 0 | 2, 2 | 1.02 | 1.04 | 8.2 | 2.0 |
| wl, every worker | workers | 2, 2 | 1.00 | 1.01 | 0.13 | 0.00 |

With no limit, probing never ends a pass full, but a nearly full cache costs 5-16 buckets a store on the workers. A
limit of 4 is the point to take. A rehash instead of the next bucket ends more passes full (Quake's core 0 36.2% at a
limit of 4).

**Two cores storing into one texel in one pass:**

- Quake III: 1 texel in 5,502 passes, a same-word race (a race today too). Doom and wl: none.
- Quake: 664 texels in 279 of 4,128 passes. 300 of them are same-word races today too; the rest would lose words under
  whole-texel commits. All of them are Quake's hand-off between core 0 and its scene worker: `scene_job` against
  `record_screen` and `SE_SceneFrame`, `seglSize` against `SE_ScreenRecord` and `Sbar_Draw`, `qglRecordBegin` against
  `qglBegin`/`qglRect`/`qglParticleRoomLeft`, `seglSwapTake` against `qgpuReplay`, `R_DrawParticles` against
  `SE_ParticlesGive`. The fix is in Quake's port: the fields each side writes go on 16-byte lines of their own, as
  gles.c's statics already are for the RISC-V machine.

**Fixed, 25 October 2026: every program with workers at 0.** `python3 gpu_vm/userland/apps/test_all.py --races` runs
each case with workers under `GVM_RACES` and fails one with a change undone or a texel two cores stored to in a pass
(`gvm_cpu` reports on SIGTERM too, for a program that does not exit).

- The ports already put every variable on 16 bytes of its own (`objcopy --set-section-alignment .data*=16`), but on gpu_vm
  that was a no-op: llvm-objcopy takes an exact section name, GNU objcopy a pattern. `gpuvm-linux-objcopy` now makes a
  pattern the input's own sections. With Quake, Quake III, ClassiCube, Doom, Tiberian Dawn and Red Alert rebuilt: Quake
  1,863 texels to 0 (its timedemo), Quake III 3,126 and 1,575 to 0, ClassiCube 4 to 8 to 0.
- The NES (linked whole, LTO) had 1: the link split the first core's aligned `listing` into words, and the emulating
  core's `frame_paced.due` lay beside them. `due` is now a volatile struct of 16 bytes (`linux/nes/osd_shaderemu.c`).
- Every case of the test set, the long ones too (140 run): 0 undone, 0 texels. `--races` covers every case, as nxpaint,
  nxweb and the IDE take workers without the RISC-V set marking them.
- The cases that read the host now run: gvm_cpu takes `--host`, `--host-from` and `--host-wheel` as rvc_cpu does (the
  eight words at the control words' 0x360) and `--fixed-dt` is ignored as there. ClassiCube's eight seat cases and its
  sound, Quake's three server modes: 0 undone, 0 texels, and their own checks pass (vr-pick with `--wall-scale 2`
  under the check: its last line came after the wall). Still skipped: `lazy` and `zero-tail`, images of their own
  with no workers.
- Programs with no case: the Dear ImGui demo (300 frames), geotune, the browser's strips (`NXWEB_CHECK=1` on two
  pages: 0 strips differ), and wl's 25 kernels that run on workers (`build/wl/img`; `-m p` and `-m w`, 1 s each, the
  sums as on one core; `phys_step` has no `inst()` and runs on one core only): 0 undone, 0 texels. Each run under 5
  minutes.
- Not a texel conflict: Red Alert's movie check (`ralert-movie`) fails on images made since 24 October (147 of 154
  frames on img-cur's kernel, 12 on img-ui5's; 0 texels, 0 undone either way). The LCW unpacker worker fills the next
  frame's pointers while the frame before is drawn; a frame whose buffer goes back to the loader before its unpack is
  waited for would give this. Not fixed.
- Half-Life had no alignment at all: 20-21 texels and 1-3 changes undone (a race today too) in hldemo1's walk and
  fight. Its build now aligns every variable as Quake's does (`linux/halflife/build.sh`): 0 and 0 in 16 sections (the
  walk's 3 to 56 s and the fight's 31 to 45 s, the drawing on its worker and `HL_RENDER=core`).
- A long program is checked by sections: `GVM_RACES_AT=TEXT` runs at full speed to where the console says TEXT and
  checks from the next pass on, `GVM_RACES_UNTIL=TEXT` ends the run there (a case's `race_at` and `race_until` in
  `test_all.py`). Half-Life's script lines (`script 24.00:`) and `HL_MARK`'s (`hlmark: begins`) bound its sections; a
  section is under 5 minutes.

## Directions not yet tried

These are not measured. They are listed for the plan (`gpu_vm/PLAN_GPU-VM.md`).

- **Lanes that share the cache instead of copying it.**
  - A core's lanes run in lockstep, so the four lanes of a quad can exchange words by `ddx_fine`/`ddy_fine`, which is
    exact at 38 ns (IDEAS 30).
  - Each lane could keep a quarter of the cache: four times the entries for the same local memory, or a quarter of
    the traffic.
  - Each lane writes only its own pixels' texels at the end, which fits a cache split across lanes.
- **Stores the compiler gathers:**
  - stack frames and structures laid out by what is stored together (AGENTS.md's Quake `particle_t`);
  - whole-texel stores (`sq`) where four words are stored together;
  - fewer callee-saved saves (IDEAS 28).
- **The stack and spills in the core's file** instead of the cache (phase 2's `lf`/`sf`, builder xr; off in c8).
