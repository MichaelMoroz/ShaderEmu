# wl: the machine's workloads, piece by piece

`wl` runs small kernels, each a piece of a real program of the machine copied from its source, for a given time and
prints how many units of work it did a second. It is built for both machines from the same sources, so one kernel's
rate on the RISC-V machine and on gpu_vm compare the machines on that kind of work. A **unit** is the kernel's own
piece of work (a frame, a server tick, a batch of traces) and is the same on both.

```
wl list
wl [-s SECONDS] [-m c0|w|p] [-n WORKERS] NAME[,NAME...]|all
```

It prints `WL name mode units seconds rate sum`. The sum is one unit's result: every unit must give the same, and a
worker whose unit differs is reported. Floats may give another sum on the other machine (gpu_vm's multiply-add rounds
twice, its special functions are the card's).

The three modes:

| mode | what runs |
|---|---|
| `c0` | the first core alone, no workers |
| `w` | the units on one worker while the first core rests in `mcw_rest_for()`, as Quake III's server runs |
| `p` | every worker (`-n`, 255 by default) runs units of its own scratch; a worker without a core is left out |

## The kernels

Each `k_*.c` names at its top the file and the functions it copies, and what its unit is.

| group | kernels |
|---|---|
| game code (`kernels_games.h`) | `q3_trace`, `q3_snap`, `q3_bot`, `q1_hull`, `doom_render`, `hl_studio`, `q1_alias`, `hl_pmove`, `doom_play`, `q3_tess`, `q1_surf` |
| rendering, physics, emulation (`kernels_render.h`) | `ray_trace`, `path_bvh`, `phys_step`, `cpu6502`, `cc_mesh`, `td_path`, `soft_raster` |
| system and libraries (`kernels_system.h`) | `media_inflate`, `media_jpeg` (libmedia itself), `q3_names`, `q3_sort`, `gzip_match`, `mem_copy` |

A kernel is a `wl_kernel` (`wl.h`): `setup()` makes its read-only tables once, `inst()` a scratch for one core, and
`run(ctx)` one unit. `run()` stores only to its scratch and its stack (it makes no system call and no allocation), so
every worker can run it on a scratch of its own. `phys_step` keeps its world in `physics.c`'s globals and runs on the
first core and on one worker only. Scratches come from 4 MB blocks used again by the next kernel; with 255 workers a
scratch must stay under about 250 KB.

## Building and measuring

In WSL:

```
bash programs/wl/build.sh host     # build/wl/host/wl, without the libmedia kernels: try a kernel here first
bash programs/wl/build.sh          # build/wl/rv/wl (rv32-cc, GCC 13.3, flat) and build/wl/gvm/wl (gpu_vm's clang)
bash programs/wl/image.sh gvm      # build/wl/img: gpu_vm's img-cur with wl (and hostwants, or the desktop starts)
```

The RISC-V image (Windows): put `build/wl/rv/wl` in `linux/prebuilt/nommu/root/usr/bin`, run
`python linux/nommu/image.py nommu_wl` with `HALFLIFE_DATA=0`, and take it out again.

On the card, through `tools/gpu_queue.ps1` (run.py's harness does that):

```
set GVM_EXTRA=--ticks 8192
python programs/wl/run.py build/wl/runs/NAME --machine rv,c8_v0001 --cmd "wl -s 3 all" --cmd "wl -m w -s 3 all" --cmd "wl -m p -s 3 all" --limit 2400
python programs/wl/plot.py build/wl/runs/NAME.html build/wl/runs/NAME
```

`plot.py` draws gpu_vm's rate against RISC-V's, a log-log panel a mode, and each kernel's ratio; several run folders
(rounds) are averaged.
