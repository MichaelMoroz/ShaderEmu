# Plan: the tick made simpler, and RAM in pairs

Written 2 November 2026, nothing of it built. All on a branch, `tick-simple`, each stage a commit or a few, `main` untouched
until the whole is measured. Numbers here are from one RTX 3090, this date. Optimizations are weighed together: a stage that
loses alone is kept aside and tried again with the later ones, not dropped.

## What came of it (2 November 2026, on the branch `tick-simple`)

Numbers are one RTX 3090. Wall a frame or tick time, against the stage before, on the shell loop, Doom's timedemo and Quake at e1m1.

| Stage | What | Result |
|---|---|---|
| 1 | fxc2 a submodule; `c` and `ot` statics under `static_noinit` | The disassembly as it was, line for line. `xr` under it: 0.6 to 0.8% slower, so it stays zeroed. `ubfe` on the whole tick: level within 0.3%, left off |
| 2 | The dead out, 362 lines of functions and the properties | Shell -3.7%, Quake -4.5%, Doom level |
| 3 | The host's timer, `csr_read`'s random number, `p.y`'s wrap | Shell -0.5%, Doom -0.8%, Quake +0.3% |
| 4 | One row of cores, geotune out | Shell -1.7%, Doom -1.8%, Quake -2.5%, `nxray` -11.6%, `nxphys` -11.7%, the commit a third cheaper |
| 5 | The ROM from its first row, RAM's image one texture | Level: these loads hardly read the ROM |
| 6a | Every way of pairing, through the mirror | Below |
| 6 | The port | **Not started**, by the rule of 6a |

**Where stage 4 differs from the plan.** RAM stays at row 64 and the field's state is in tiles 5 and 6 of the slots, which no row had to move for. Workers are 2^5 at most, so decision 2 is 2^5.
A worker's texels are in tiles, not rows (rows: Quake 50% slower). After the stage, one triangle over the targets with what no core runs discarded (Quake -0.5%, `nxphys` -2.7%, `nxray` -5.7%; a warp is 4 x 8 pixels of one core, by `InterlockedAdd`) made `TICK_GAP` needless (decision 3: Quake +0.4% without, 26% under quads), so the targets are 2048 x 16.
Sparse blocks, by bit reversal, were level. The old paths without `RAM_INPLACE` in `main.shader` went; `rvc_trace12`, on D3D12, still expects them and is not known to run.

**Stage 6a.** Percent of tick time against the mirror in rows, only the active tick's mirror drawn. The figures earlier in this plan, -4.3 and -4.6, had the mirrors' draw order in them.

| Way | Shell | Doom | Quake | Quake III | `nxray` |
|---|---|---|---|---|---|
| Pairs, rows 2048 wide | -1.5 | +1.6 | -1.9 | -0.5 | -3.8 |
| Pairs, no `64 +` | -2.2 | +1.0 | -1.7 | -0.9 | -6.2 |
| From an odd row (the control) | +0.8 | +1.8 | +1.0 | +1.0 | -1.6 |
| Down, then along | -1.6 | +0.9 | -1.3 | -0.4 | -5.6 |
| Rows 1024 wide | -1.5 | +1.0 | -1.7 | -0.5 | -0.9 |
| In strips | -0.4 | +3.0 | -0.8 | +0.8 | -5.6 |
| Tiles 2 x 2 | -1.0 | +2.0 | -0.4 | +0.1 | -8.8 |
| Tiles 2 x 4 | +0.6 | +3.7 | -1.0 | +0.6 | +2.4 |
| The map for fetches only | -0.6 | +1.5 | +0.1 | +0.3 | +0.3 |
| For loads only | -0.4 | -0.2 | -1.1 | -0.9 | +6.8 |
| Rows, no `fw1` | +6.9 | -1.2 | +6.1 | +1.9 | +11.5 |
| Pairs, no `fw1` | +1.3 | -0.6 | +1.0 | +0.4 | +5.5 |
| Pairs, a load keeping the other texel | +1.1 | +2.3 | +1.7 | +3.5 | -1.3 |

Half-Life's data is not in an image made without WSL, so it is not among them. The pair's arithmetic in the commit and `ram()`, landing on today's addresses, cost nothing measurable (`nxray`, Doom, Quake, Quake III level), though no test here has the commit writing pairs.

**Why no port.** On the games the best simple map is Doom +1.6, Quake -1.9, Quake III -0.5: about nothing, and the rule was that stage 6 does not start then. The port touches every device row, host readback and Unity pass, and AMD and Intel may gain nothing. Worth trying again if a program that matters looks like the shell loop or `nxray`, or the fetch changes. The mirror is in git nowhere: it is `RVC_MIRROR` in a patch, and is simple to make again from this.

**Not done or not known.** No Unity project is on this computer, so nothing of Unity was compiled or run: the editor scripts, `EmuMachine` and the shaders need "Sync shader sources", "Import boot images" (`_ram.texels`), the world built anew and fxc2's `bin/unity`. The kernel was not rebuilt (no WSL): nothing needs it, but `linux/userland` and `programs/mc` changed in source only (`geotune` out of `emuinit`), and the prebuilt `emuinit` still looks for it and goes on. Decision 1 needs an AMD and an Intel card, so the ROM stays RGBAFloat. Nothing here ran on such a card.

## What is known

**Dead** (read in the code, and for the device tree run):

| What | Why it is dead |
|---|---|
| `_Data_DTB_R/G/B/A`, the `0x1020` branch, `rom4`, `unpack` | The kernel gets the tree from RAM (`a1 = 0x83e00000`). A tick without them booted to a shell on WARP |
| `_InitRaw`, `_Data_RAM_RAW` | Every host sets 0, none gives the texture. The tick tests it per pixel |
| `_TicksDivisor` | 1 in the harness, unset in Unity. A division on every pixel |
| `_DoTick`, `_UartBurst`, the commented-out debug properties | Not read by any shader (`_UartBurst`: a default the harness reads) |
| Six functions of `helpers.cginc`, `decode`, `xreg_load`, `xreg_store`, `fp_state_load` and others of `types.h`, 15 macros of `l1_local.h` | No use by name. Compile time only |
| `geotune` | The init starts it and it returns at once: the kernel has laid out the workers. Nothing runs `geotune again`. The hybrid geometry, `--workers` and the world's "Worker cores" presets go through it |

**Dear for no reason:**

- `load_state` does doubles on `_Time.x` on every pixel in Unity. The integer clock exists behind `NO_DOUBLES`.
- The ROM is upside down: every read is `GetDimensions`, a modulo and a division. Its width is always 2048 and we make it.
- `geo_read`, `select_core` and `core_in` divide by variables per pixel; `tile_at` divides by 248 for every state texel a worker
  reads, about 20 of them per pixel in `load_state` alone.
- `xr[65]` is zeroed every pass, then overwritten (65 instructions).
- `csr_read` works out its random number for every CSR read; `p.y %= ...` wraps what should be in range. Not measured.

**The compiler.** `#pragma static_noinit` (fxc2's patch 62) is in the FXC2 checkout's DLL and not in `tools/fxc2`'s. With the
checkout's DLL, `c[C_N]` as a static has the tick's size and line count of today (180,456 bytes, 6,992 lines), against 448
zeroing instructions a pass without the pragma. `xr` under it: 178,896 bytes, 6,927 lines. FXC is out of the tick's path.

**RAM's layout.** A cold miss brings 32 bytes, the texel and the one below it, so rows waste half of every miss (`codemap --line`
on this card: 213 ns a step for the neighbour at dy 1, 316 to 327 for every other within 16 x 8, dx 1 too). Measured on the
real tick through a mirror (RAM redrawn into a texture of each map before each tick; the commit as it is), percent of tick
time against the mirror in rows, the active map's mirror drawn first:

| Map | Shell | Doom | Quake |
|---|---|---|---|
| Strips 512 wide | +1.0 | +1.7 | +1.4 |
| Pairs in strips | -3.6 | +1.5 | -3.4 |
| **Pairs, rows 2048 wide** | **-4.3** | **+0.8** | **-4.6** |
| The same, `VKD3D_UBFE=1` | -5.0 | +0.2 | -5.6 |

Pairs: `x = (t >> 1) & 2047`, `y = 64 + (((t >> 11) & ~1) | (t & 1))`. Noise 0.0 to 0.2%. The commit, the GPU's `ram()` and
the host pay nothing in this test, and they will. The ROM's width makes no difference (2048 to 16384 wide, same bytes).

## Stages

Every stage ends with `tools/test_all.py`, `tools/test_card.py mctest-3 --warp --parts --max-wall 240 --mark-ok`, and `lean/tick.hlsl`
the same bytes as `tick.hlsl`. `--races` where workers change.

**Measuring.** The shell loop, Doom's timedemo and Quake at e1m1, from one snapshot each, on the card. While the layout is the
same: `RVC_TICK_ALT`, in one run. A stage that moves the layout cannot use it, and a snapshot made by each build measures
nothing, so stage 0 makes the harness lay an old snapshot out anew as it loads (as `origin/rv-ram` did), and those stages run
A B A B from one snapshot.

### 0. Ground

1. Branch. The three snapshots, kept outside `build` so nothing cleans them.
2. Snapshots laid out anew on load, behind a version in the magic: every layout stage needs it.
3. `RVC_MIRROR` is an experiment and stays out of the tree (`mirror.patch`, kept beside the numbers).

### 1. The compiler and the arrays (no layout change)

1. `tools/fxc2` becomes a git submodule of `https://github.com/MichaelMoroz/FXC2`, following its only branch (`branch = master` in
   `.gitmodules`; it has no `main`), in place of the copied DLL and README. A submodule still pins a commit: a new compiler is a commit here that
   moves it, with its measurements. The DLL is then `tools/fxc2/bin/d3dcompiler_47.dll`: `CMakeLists.txt` (lines 54 and 61) with a
   clear error when it is missing, `docs/machine/fxc2.md`, the tick skill and `AGENTS.md`, which name `Documents\FXC2` as the source:
   the source is now the submodule. `README.md`'s setup says `git submodule update --init`, as do the worktree notes of the merging
   skill. Keep the LGPL note of the old README where the DLL is described. `fxc2_check.h` raised to what the tick needs.
   Compile, compare the bytes with the old DLL's. Check FXC2's README shaders still compile: Poiyomi, Unity's.
2. `c` and `o` static under `static_noinit`, their `inout` arguments gone. Must be level: if the bytes differ, why. (`o` failed
   to compile in a scripted try, where `o` is also a local name: rename.)
3. `xr` under `static_noinit`. On `gpu_vm` this was mixed, so three loads decide.
4. `VKD3D_UBFE=1` for the whole tick, on its own, alternating: it helped each map within a run and was never compared to the
   tree on one clock. The compiler's switch is process-wide, so A B A B.

### 2. Dead things out (no layout change)

1. The device tree's textures, branch and functions; `loadPayload` of `dts`; `--dtb`; Unity's properties and `EmuMachine`'s
   binding. `dts.bin` stays, in `linux_payload.bin`. The name scan above is re-run after each removal.
2. `_InitRaw` and `_Data_RAM_RAW`; `_TicksDivisor`; `_DoTick`, `_UartBurst`, the commented properties.
3. Dead functions and macros. Then the paths without `RAM_INPLACE` and `NO_PAGING` in `main.shader` and `types.h`, traced first:
   what is left of `types.h` should be what the commit calls.

Taking code out has moved the tick 1 to 3% before, either way. Measured as one, then by part if it moved.

### 3. Costs on every pixel (no layout change)

1. The clock from the host always (`NO_DOUBLES` the only path), Unity's `_HostMtime` with it. The double's rounding is the
   clock's behaviour: a world's time must still run true.
2. `csr_read`'s random number only where it is asked for; `p.y %=` gone if the geometry says so.
3. `tile_at`'s division once a core, if stage 4 does not make this moot: try it, since it shows what stage 4 is worth.

### 4. One row of cores (layout change)

Cores wider instead of taller. Each core a block of 8 x 8 pixels of the targets: `core = x >> 3`, `pixel = p & 7`, no division by
a variable. 256 x 8 is 2048 wide, 2056 if `TICK_GAP` stays.

1. `geotune` out, with `MC_HYBRID`, `MC_UNIFORM`, `--workers`, the host's preset bits and "Worker cores". Every worker alike, as
   at every boot now. `nxray` and `nxpath`'s picture keep today's speed; the dense layout, which nothing asks for, is gone.
2. Geometry in `tick.hlsl`, `types.h`, `gpu.h`, `ram.h`, the kernel, `programs/mc`, `rvc_cpu`'s `geometry[]`, `MachineBlit.cginc`,
   `EmuMachine`, `tickWidth`/`tickHeight`. State: a core's slot 8 tiles at `((k & 31) * 64, (k >> 5) * 8)`, which is all of rows 0 to 63.
3. The field's state moves, its rows 48 to 63 being taken: state rows 64 to 79, RAM from row 80. The target needs rows for the
   field: 2048 x 16 more gives 4,096 lanes (now 3,968).
4. One core a wave: look at the waves with the draw as quads per core (once, a quad's diagonal across blocks mixed 15 to 22%).
   One triangle over the row, idle cores leaving at the top, was clean and cost nothing while the busy ones lie together.
   `waves` lived in `gpu_vm` (removed in `3f0affd2`; in git).
5. Measure with `nxray`, `nxphys`, `play-test.mp3` on 255 workers too, and the three loads.

### 5. The ROM and RAM's image (layout of the ROM)

1. The ROM top-down, read by a mask and a shift, no `GetDimensions` (the tick, `gpu.h`, `types.h`, `ram.h`, the world's shaders,
   `createTextureTexels`, the importer). `texture_texel` one load, not four words; `ram.h:624`'s copy too.
2. RAM's start image in one texture like the ROM, not four. **The Udon model reads `ramLanes` and `romLanes`:** its import stays.
3. Format: see the decisions.

### 6a. Every way of pairing, through the mirror (no layout change)

Before the port, and on the tree of stage 5: the mirror measures a map in a few minutes, the port in days. `mirror.patch` on the
branch only, never in `main`. All against the mirror in rows, the active map's mirror drawn first, on the three loads and on
snapshots of Quake III, Half-Life and `nxray` (Doom's loss may be its own, and three loads are few). With and without `ubfe`.

| Way | What it asks |
|---|---|
| Pairs in rows 2048 wide | The one to beat |
| Pairs down, then along (a column of pairs 4,096 tall) | The same sector, other arithmetic |
| Pairs from an odd row | Whether a sector starts on an even row only (the control: should lose its gain) |
| Pairs with no `64 +` (state after RAM, or RAM from row 0) | A step less on every read |
| Pairs in rows 1024 wide | As much of RAM as 16,384 rows hold; a snapshot that fits |
| Pairs in strips, tiles 2 x 2, 2 x 4 | Whether anything larger pays yet, now on the real tick |
| The map for fetches only; for loads only | Which side gives the gain, and whether Doom's loss is its loads |
| `fetch` without `fw1`, or taking the pair's second texel without a test | The eager next texel may be cheap or needless once it is in the same line |
| A load remembering the pair's other texel beside `dr_t` | The same for data |

The last two are changes to the fast loop: the three loads, and the shape rules of `tick-costs.md`. Ways are weighed together: a
map that loses alone is tried with the fetch and load changes.

**Other cards.** The winner, rows and a control run on an AMD and an Intel card, and `codemap --line` for the sector's shape. If a
card fetches another shape, or pays the arithmetic for nothing, that goes into stage 6's verdict: the machine has one layout for
every player. Needs someone with such a card, as decision 1 does; the same run can answer both.

Stage 6 ports only the winner, and does not start if nothing here beats rows on the games.

### 6. RAM in pairs (layout change, the large one)

Through `RAM_ADDR`, which all reads and points share; `origin/rv-ram` (50 files) lists the places rows are assumed.

1. The tick; `types.h`'s `RAM_ADDR` and `RAM_LIN`; `gpu.h`'s `ram()`, `sound.h`, `zap.h`, the gather.
2. `ram_rows` and `ram_quad`: a pair of rows is 4,096 texels. A range's ends can be odd, one pixel of a row. Fills and copies
   out of the ROM, the picture and the zap's square.
3. Control rows and mailboxes (`CONTROL_ROW`, `kMboxRow`), the harness's zones, uploads, readbacks, watches, memory view, snapshots.
4. Unity: `MachineCommit*`, `MachineBlit.cginc`, `Machine.shader`, `EmuMachine`, Sound, Display. `rvc_cpu`'s uploads.
5. **Verdict on the whole frame**, not the tick: the mirror charged nothing for steps 1 to 3. Compare frames a second in Doom,
   Quake, Quake III and Half-Life, and `RVC_PASS_PROF`'s parts. Doom draws through `ram()` and is the one to fear. Kept only if
   the three loads and the games say so; else the stage's commits stay on a branch and the result goes in `tick-costs.md`.

### 7. Into `main`

World: "Sync shader sources", a boot, `totalInstructions` with the Game view in front, change and tree in turn. Then `AGENTS.md`
(geotune, the layout numbers, the new rules), `shader_vm/README.md`, `docs/machine/multicore.md`, `gpu.md`, `nommu.md`, `lanes.md`, `tick-costs.md`,
`fxc2.md`. `linux/prebuilt` if the kernel moved, which needs WSL (none on this computer on this date).

## Decisions

| | Question | Going in with | Settled by |
|---|---|---|---|
| 1 | ROM as RGBA8 (8192 wide, no float risk) or RGBAFloat (one load a texel)? | RGBAFloat | A bit-pattern check on an AMD and an Intel card. RGBA8 is 42 to 70% slower on whole texels, 4 to 7% faster on word runs, here. Any lost bit decides for RGBA8 |
| 2 | Workers of 2^5 in an 8-tile slot, or 2^6 filling it? | 2^5 | Stage 4, by stores a pass against the commit's points |
| 3 | `TICK_GAP`: is it needed? | Dropped, for 2048 exactly | Stage 4's waves. It is not known why it is there |
| 4 | Pairs of 2048 or narrower? | 2048 | 1024 needs 18,432 rows, over the limit of 16,384, unless RAM shrinks |

## What this does not know

Everything here is one NVIDIA card. The 32-byte sector is that card's, and AMD and Intel may gain nothing from pairs and pay their
arithmetic. Worth a look before stage 6: the mirror's patch runs anywhere, so someone with such a card can give the row above.
