# rvc_opt

A patched copy of the shader from [pimaker/rvc](https://github.com/pimaker/rvc) (`_Nix/rvc`,
MIT License, Copyright (c) 2021 PiMaker). The state layout and the Commit pass are untouched,
so it runs upstream snapshots and payloads, and its emulation is bit-identical to upstream.

    tools\perf_test.ps1 -Rvc experiments\rvc_opt
    bin\rvc_harness.exe --rvc experiments\rvc_opt --payload rvc\_Nix\rvc\data-net --load-state build\snapshots\rvc_shell.snap

## Changes (all in the CPUTick pass)

Each one removes work that upstream repeats on every emulated instruction. All added state is
pass-local (`static`), never stored in the texture.

1. **Timer hoisted** (`emu.h`, `main.shader`): `mtime` depends only on `_Time`, so the
   double-precision math runs once per pass instead of once per instruction.
2. **Hot CSR shadows** (`mmu.h`, `csr.h`, `trap.h`, `emu.h`): `mstatus`, `mip` and `mie` were
   each looked up per instruction (a 16-way compare chain, then a texture read). They are now
   read once and dropped on any CSR write.
3. **One-entry TLB per access mode** (`mmu.h`): a successful fetch/read/write translation is
   reused while the page number repeats, instead of a two-level page walk (two dependent
   texture reads) per access. Flushed on any CSR write, `satp` change, privilege change, trap
   and `sfence.vma`. Only successful translations are cached, as RISC-V permits.

## Measured (RTX 5090, `tools\perf_test.ps1`, 2048 ticks)

| Shader | IPS | State hash after 1,024,000 instr |
| --- | --- | --- |
| upstream | 517k | 8d6fc1106d9cd3a1 |
| 1 + 2 | 604k | 8d6fc1106d9cd3a1 |
| 1 + 2 + 3 (this folder) | 748k | 8d6fc1106d9cd3a1 |

A scripted shell session (5.07 M instructions, with traps, syscalls and cache stalls) also ends
in the same state and UART output as upstream. Cold boot to the prompt: 78.5 s -> 52.9 s.

## ddx_fine / ddy_fine quad sharing: works, not worth it for rvc

- FXC accepts fine derivatives inside the `[loop]` tick loop and under `[branch]`, with warning
  X3595 only. `[fastopt]` on the loop must become `[loop]`, or FXC tries to unroll and fails.
- A probe that sends every RAM read result through two x-neighbour round trips (16-bit halves
  as floats) stayed bit-identical, so the exchange is exact on this GPU.
- It cost 9% (471k vs 517k IPS). What it could buy is write-cache capacity, and rvc is not
  short of it: the benchmark never stalls, and a whole boot loses under 2% of ticks to stalls.
- Layout catch: lanes of a quad must all run the CPU. Upstream's state region starts and ends
  mid-row, so some quads have lanes that return early; a real version needs a quad-aligned
  state area.
