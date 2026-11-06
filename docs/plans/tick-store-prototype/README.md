# Tick prototype: atomics through the store's call, one store site in the general step

Status on 5 November 2026: a prototype. Idea 1 is stamped (`warp_ok`) and measured on Quake e1m1 and Tiberian Dawn:
it loses there (+1.6 to +3.5% and +2.0%) while the shell loop gains 4.3%. Not in Unity's copy. Do not merge as it is.

6 November 2026: the branch `tick-atomics-through-store` was deleted and this folder kept in main, the tick's change as
`idea1-atomics-through-store.patch` (against 62c2cd86's `shader_vm/tick.hlsl`). The tick has had five fast-loop changes
since (docs/machine/tick-costs.md, sixth round): the patch will not apply as it is, and any figure below is of 62c2cd86.

## What was on the branch

- `shader_vm/tick.hlsl` (`idea1-atomics-through-store.patch` here): **idea 1**, atomics through the store's call site, plus a `FROZEN_WORD` hook for probes.
- `idea2-one-store-site.patch` (this folder): **idea 2**, one store site in the general step. It applies with `git apply`
  on top of the tick here or on the tree's.
- `warp_ok` holds idea 1's stamp `42ae1d98014e235d` (`tools\warp_stamp.py --count 2`: 2 of 2 passed, 279 and 286 frames
  in 6 s on WARP).

### Idea 1: atomics through the store's call site

The fast loop's separate atomics block is gone. Group 2's test also takes `(w & 0x707f) == 0x202f`, and the store branch
is `opc == 0x23 || opc == 0x2f`, with `amo = (w & 8) != 0`:

- The address is `a` for an atomic and `a + imm_s` for a store.
- For an atomic, `ok` is the old atomic test: on a word, in RAM, and funct5 under 16. For a store it is the store's test.
  FAST keeps its own forms.
- `ram_store_nc` became `ram_store_op(o, v, mask, op)`, and `ram_store_nc` is now a wrapper with `op` 0:
  - op 0 is a store under the mask;
  - op 1 keeps the word (lr, or an sc that fails);
  - op 2 + funct5 makes the word from the old one (add, xor, or, and, swap) in the partial branch.
- The old word comes back in the static `st_old`. An atomic passes mask 0, so it always takes the partial branch.
- After the call, by selects:
  - `res_en` and `res_addr` change only when `amo && ok`;
  - `v` is the sc result or `st_old`;
  - `dst` is 64 for a store only.

### Idea 2: one store site in the general step

`step()` keeps `st_a`, `st_v` and `st_n` (0 means no store). Its stores and atomics set them instead of calling
`store_any`. One `[branch] if (st_n != 0) store_any(st_a, st_v, st_n);` comes after the tree, before `xr[dst] = v`.
This saves one inlined `ram_store_nc` copy, about 286 DXBC instructions.

`rvc_cpu` needs nothing for either idea: the machine's behaviour is the same.

## Measurements (RTX 3090, 4 to 5 November 2026)

The ticks were measured from copies of the main checkout's `shader_vm`. That checkout had uncommitted changes in
`src/gpu.h` and `src/types.h` (GPU work in progress), so the baseline is "tree + those". Re-measure on the other
machine against its own tree.

### Shell loop

All ticks ran in one run from one snapshot:

```
RVC_TICK_ALT='build\shader_vm_i1;build\shader_vm_i2'
pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc build/shader_vm_base --define NO_PAGING
  --image build/images/nommu --no-desktop --no-viz --no-stdin --load-state SNAP --stats-after 5 --seconds 45
```

Results, ms a tick:

| Run | Tree | 1 + 2 | 1 only | 2 only |
|---|---|---|---|---|
| Tree first | 1.2886 | 1.2364 (-4.1%) | | |
| Order swapped | 1.2826 | 1.2289 (-4.2%) | | |
| Three ticks in one run | 1.2852 | | 1.2294 (-4.3%) | 1.2854 (level) |

Two copies of one tick differ by about 0.03%.

### Frozen probes

The probes used `RVC_FREEZE=1 --bench 500 --seconds 4` and `--define FROZEN_WORD=X`, with two alternating runs each.
Figures are ms a pass (fastest frame):

| Word | X | Tree | 1 + 2 |
|---|---|---|---|
| addi a0,a0,1 | `0x00150513` | 0.792 | 0.845 (+6.6%) |
| lw a5,0(sp) | `0x00012783` | 1.199-1.205 | 1.251 (+4.4%) |
| sw a5,0(sp) | `0x00f12023` | 1.226 | 1.488-1.492 (+21.5%) |

Under freeze, `RVC_TICK_ALT`'s per-tick timer records 0 frames, so run each copy on its own.

The probes and the shell loop disagree. The probes compile a different shader (the define), and they move with the
driver's modes (`tick-costs.md`, "Fifth round"). But the store branch now does an atomic's work on every store, so a
store-heavy load (Doom) may lose.

### Correctness

`tools\warp_stamp.py` passed on the copies:

- 1 + 2: two snapshots of `nano-X; glxgears & mctest 3`, whose workers use lr/sc and amoadd;
- 1 alone and 2 alone: one snapshot each.

### Games (second machine, 5 November 2026)

Measured against that machine's clean tree at 62c2cd86 (no uncommitted changes), RTX 3090, both ticks in one run
(`RVC_TICK_ALT`), 45 s from one snapshot each. Doom was left out by the developer's standing choice; Tiberian Dawn
stands in for it as the second game.

Snapshots (`--cpu ... --save-state`, image `build/images/nommu`):

- Quake: `nano-X -p & sleep 2; QUAKE_TRACK=5 quake +map e1m1`, `--until "track 40:"`.
- Tiberian Dawn: `nano-X -p & sleep 2; TDAWN_AUTO=1 TDAWN_SEED=7 TDAWN_NO_DELAY=1 TDAWN_SCEN=3 TDAWN_SUMS=1 tdawn
  FROMINSTALL`, `--seconds 110` (frame 2,113 of scenario 3; it prints a sum a frame).

Results, ms a frame's tick:

| Load | Order | Tree | Idea 1 |
|---|---|---|---|
| Quake e1m1 | tree first | 1.7357 (3,434 frames) | 1.7966 (3,377 frames): +3.5% |
| Quake e1m1 | idea 1 first | 1.7298 (3,481 frames) | 1.7581 (3,431 frames): +1.6% |
| Tiberian Dawn | tree first | 1.4490 (4,098 frames) | 1.4785 (4,120 frames): +2.0% |

So idea 1 as it is wins only the shell loop: the store branch's extra work costs the games more than the atomics block
saved. The frozen sw probe (+21.5%) said as much.

## Next

1. Done: stamped.
2. Done: Quake and Tiberian Dawn, idea 1 slower on both (above).
3. Only worth trying if plain stores can pay nothing for it:
   - compute `sc_ok`, `sop` and the `res_*` selects only under `amo`;
   - or keep `st_old` out of the whole-word path.
   Then run `test_card.py mctest-3 fptest`, copy to `unity/ShaderEmu/Shaders/lean/tick.hlsl`, and add a row to
   `docs/machine/tick-costs.md`.
4. Idea 2 is level on the shell loop. Keep it only if Doom or Quake says so; otherwise drop it.

### Recreating the setup

The copies live under the gitignored `build/`: `shader_vm_base` (the tree plus the hook), `shader_vm_try`,
`shader_vm_i1` and `shader_vm_i2`. To recreate them:

1. Copy `shader_vm` to the four folders.
2. Delete `warp_ok` in each.
3. Put this branch's tick in `i1`, and the tree's with the hook in `base`.
4. Apply the patch to make `i2` (from the base) and `try` (from `i1`).
5. Stamp each folder.

The shell-loop snapshot was made with:

```
pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --cpu --rvc shader_vm --define NO_PAGING --image build/images/nommu
  --no-desktop --no-viz --no-stdin --expect "# " --send "sh -c 'i=0; while [ $i -lt 2000000 ]; do i=$((i+1)); done'\n"
  --seconds 25 --save-state build/tick_try/shell.snap
```
