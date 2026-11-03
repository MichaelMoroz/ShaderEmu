# Compiler flags: every build against what was measured to pay

What paid on the ports (`porting-games`, "Compiler flags that mattered"; `optimizing-games`, section 7):
`-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops` (3 to 6% fewer instructions than `-O2` on Doom
and Quake), every global hidden (`nommu_hidden.h`, put in by the flat drivers for everything), LTO with
raised inline limits for the large games, `-fno-math-errno -fsingle-precision-constant` where floats are
the work, played counts (PGO) for the games that have them, and `-fno-common -fdata-sections` with 16-byte
data sections for what workers write. This page goes through every build of the no-MMU machine
(3 November 2026) and says which of those each has, and why the ones without do not need them.

## How it was looked at

- The flags each script gives, read from the scripts (`linux/*/build.sh`, `linux/nanox/*.sh`,
  `linux/userland/*.sh`, `linux/nommu/setup.sh`, `linux/ports/*.sh`).
- **Doubles by mistake**: every program of the desktop and the window system disassembled, and its calls
  to the soft-double routines counted by function, the C library's own (`printf`'s, `strtod`'s) left out.
  The machine has no doubles: a `float * 0.5` without `-fsingle-precision-constant` is two calls. None
  of the programs has such calls where it does its work: `glxgears`' `gear()` (188 calls, the demo's
  geometry made once at its start), `nano-X`'s `GrCalcTransform` (27, a call Nano-X's clients do not
  make), `cabinst`'s progress (24), `nxmicropolis`'s yearly score (30), `nxcalc` (doubles by design),
  the float libm's large-argument paths. Not changed.
- **Speed**: a change that could move it was measured by the instructions of a fixed run on `rvc_cpu`
  (`--count-from` a mark to the end, `RVC_THREADS=1`), the program built both ways into one image, and
  kept only where it did not lose and every sum the program prints stayed the same. `rvc_cpu` has no
  lanes: what a change does to `nxray` on the lanes is not measured.

## Changed

| Program | Before | After | Fixed run | Instructions | Sums |
|---|---|---|---|---|---|
| `nxmicropolis` (`linux/micropolis/build.sh`) | the engine at `-O2 -fwrapv` | `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -fwrapv` (objects of other flags made again) | `MICROPOLIS_SUM=1,500` and `haight.cty,1000` | 355.8 million to 229.5 (-35.5%) | `e14d1a50`, `909517ac` as before |
| `nxray` (`linux/raytrace/build.sh`) | `-O2` | `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops` | `RAY_FRAMES=4` | 148.6 million on all cores to 140.3 (-5.6%); the busiest worker 738,772 to 707,890 | the four pictures' as before |
| `nxphys` (`linux/physics/build.sh`) | `-O3 -fsched-pressure` | and `-fno-tree-vectorize -funroll-loops` | `PHYS_WORKERS=0 PHYS_SCENE=1 PHYS_STEPS=120` | 157.9 million to 157.5 (-0.2%) | `b28b77b4` as before |

Micropolis's engine was the one large program of the tree still at `-O2`; its simulation is loops over
the map's cells, which unrolling and inlining at `-O3` take apart.

## The table

| Build | Flags | Verdict |
|---|---|---|
| The flat drivers (`linux/nommu/setup.sh`: `rv32-cc`, `rv32-c++`) | `-march=rv32imaf -mabi=ilp32 -fno-stack-protector -ffp-contract=off -fPIE`, `nommu_hidden.h`, `-ffile-prefix-map` | Right: PIE as the flat build is, globals hidden, no FMA (the tick's float results are its own, and sums are compared with the build computer's). `flatcrt.o` is `-O2 -fPIC`: the start file, a few instructions once. |
| musl (`linux/userland/toolchain.sh`) | `-O2 -std=c99 -ffreestanding -fexcess-precision=standard -frounding-math -fno-strict-aliasing` | musl's own required flags; its `memcpy`/`memset` are ours (`-O2 -fPIC -ffreestanding -fno-builtin`, the commit's copy past 4 KB). `-fPIC` there is the same code as `-fPIE` (no globals). Not changed: every program's bytes and `cc-bytes` would move for nothing measured. |
| The float libm (`linux/userland/libm`), `libcompiler_rt` (`rt/`) | `-O2` | Small leaf functions; the double routines are our own fast paths (`dfast.c`). Left. |
| `emuinit`, `emumux` (`linux/userland/build.sh`) | `-Os` | Run once, size is all. Right. |
| BusyBox (`linux/userland/busybox.sh`) | its own `-O2` | The shell's commands; the shell loop is measured as the tick's load, not as the shell's code. Left. |
| Nano-X (`linux/nanox/config`, `build.sh`) | `OPTIMIZE = -O2`, server and client library | A candidate, not changed: the same flag builds `libnano-X.a`, which the machine's compiler links (`cc-bytes` moves), and the server's work is mostly the GPU's lists; to measure with `nxbench` first. |
| `glxgears` | `-O2` | 188 soft-double calls in `gear()`, once at the start: not worth a picture that differs. |
| The desktop's programs (`linux/apps/build.sh`: Files, Edit, Paint, Web, View, ...) | `-O2 -Wall` | Event-driven, the work in libmedia (decoders at `-O3 -funroll-loops` already) and the GPU. The browser's layout is the one heavy part: a candidate, measured by `NXWEB_DUMP` runs, not done. `nxcalc` and `nxsolitaire` take `-lm`. |
| libmedia (`linux/libmedia/build.sh`) | decoders `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`, `-fno-math-errno -fsingle-precision-constant` | Right. |
| `nxplay`, `nxvideo` (`linux/play`, `linux/video`) | the ring and windows `-O3 ... -funroll-loops` | Right. |
| `nxray` | now `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops`, `-fsingle-precision-constant -fno-math-errno` | Changed (above). |
| `nxpath` (`linux/pathtrace/build.sh`) | the tracer `-O3 ... -funroll-loops -fira-loop-pressure`, LTO; `nxpath.c` (the window) `-O2` | Right: the window's file is not the work. |
| `nxphys` | now `-O3 -fsched-pressure -fno-tree-vectorize -funroll-loops`, floats | Changed (above). |
| `nxchess` (`linux/chess/build.sh`) | `-O3 -fno-tree-vectorize -fsched-pressure` | Right (integer search). |
| `imdemo` (`linux/imgui/build.sh`) | `-O3` + tune, LTO, `-fno-math-errno` | Right. |
| `nxmicropolis` | now `-O3 ... -funroll-loops -fwrapv` (engine), the window `-O2` | Changed (above). |
| Doom (`linux/nanox/doom.sh`) | `-O3 -fno-tree-vectorize -fsched-pressure -funroll-loops -flto`, counts | Right. |
| Quake, Quake III, Half-Life | `-O3 ...` with raised inline limits, floats, counts, `-fno-common -fdata-sections` | Right. |
| ClassiCube | `-O2` with its hot files `-O3 ...`, floats, counts | Right. |
| Tiberian Dawn, Red Alert | `-O3 -flto ... 1to1 -fno-toplevel-reorder` with raised limits, counts | Right. |
| OpenTTD | its GCC, LTO `1to1`, `openttd.profile.gz`, 16-byte data sections | Right. |
| The NES | `-O3 ... -flto`, counts | Right. |
| QuickJS, MicroPython (`linux/quickjs`, `linux/micropython`) | `-O2` | Interpreters: candidates for `-O3` measured by a script's run; not done. |
| The IDE, the terminal programs, the ports (`dropbear`, `curl`, `socat`, ...) | `-O2` | Not where the machine's time goes. Left. |
| The machine's compiler (`linux/tcc/build.sh`) | `-O2`, its libraries as they are | Its outputs must stay the same bytes (`cc-bytes`). Left. |
| `-g` (`setup.sh`, OpenTTD, Tiberian Dawn, Half-Life) | with `-O2`/`-O3`, stripped in the image | Debug information changes no code. Right. |
| `-O1` (Age of Empires, Dear ImGui's one file) | | Age of Empires is another agent's; ImGui's is its build's own choice for one file. Left. |

## Checking it

`tools/test_all.py micropolis nxray nxphys-none` give their sums as before; the runs above are in this
page's numbers. What `-O3` does to `nxray` on the lanes of the card is not measured (no card runs).
