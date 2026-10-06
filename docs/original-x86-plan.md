# Windows in a VRChat Shader — Project Handoff

> Superseded. The project is now ShaderEmu: a bare-metal RISC-V machine tuned for speed, with Doom and an emulated GPU as the goal (see the README). An 8086 was judged too slow. Kept for its analysis of the platform and of rvc.

Oct 5, 2026 · @Mykhailo

## Summary

The recommended target is an Intel 8086 PC emulated in a VRChat pixel shader, running MS-DOS and Windows 3.0 in real mode, with an emulated 8514/A graphics accelerator whose drawing runs as parallel shader passes. Windows 95 is out of reach: it needs a 386 core and roughly 100× more instructions to boot.

What this document captures, from a design discussion on 4–5 October 2026:

- **Feasibility.** pimaker's rvc emulator already boots Linux on RISC-V in a VRChat pixel shader at up to 250k instructions per second. An 8086 machine is a smaller target in most ways that hurt rvc: 1 MB of RAM instead of 64 MB, 16-bit math, no MMU.
- **Why Windows 3.0 real mode.** It is the newest Windows that runs on an 8086, so the same CPU core serves DOS and Windows. Windows 1.0 and 2.x run on it too.
- **The main bottleneck is memory writes, not instruction decoding.** Most of the architecture below exists to get writes from the serial CPU into the RAM texture cheaply; rvc's source shows a proven way to do it (a hashed write cache, committed by gather).
- **Graphics are where the GPU's parallelism pays off.** A real 1990-era PC drew everything with the CPU. An emulated accelerator moves fills and blits into parallel passes and keeps screen traffic out of the CPU's write path.
- **Content.** MS-DOS 1.25, 2.0 and 4.0 are MIT-licensed, as are Zork I–III and GW-BASIC, and GEM (GPL) offers a legally shippable GUI. Windows 3.0 itself is proprietary, so it cannot ship inside the world.
- **Development.** The project is well suited to LLM agents because correctness is checkable against reference emulators and per-opcode test suites. The key enabler is a headless D3D11 harness that runs the shader outside Unity.

## Platform constraints

VRChat allows only graphics shaders under DirectX 11 (Shader Model 5, compiled with FXC), so all emulation state must live in render textures and be rewritten each pass.

**What is unavailable**

- Compute shaders and UAVs (unordered access views, which would allow arbitrary writes from a shader).
- Wave or subgroup intrinsics, which arrived in Shader Model 6.
- 64-bit integer math. rvc emulated the upper half of 32×32 multiplies with doubles; an 8086 never needs it.
- Real function calls: HLSL inlines every function at every call site.

**What is available**

- **Persistent state through feedback loops.** A Custom Render Texture (CRT) or camera loop reads its own previous output, so each pass reads the last frame's texture and writes a new one. A fragment shader writes one 128-bit pixel (4 × 32-bit channels) per invocation, at the end of execution only.
- **Partial updates.** Unity re-renders and buffer-swaps only the updated region of a CRT, so a small state area is cheap to iterate.
- **Vertex texture fetch and geometry shaders.** A geometry shader can emit points at arbitrary pixels, which gives a scatter write (see Shader architecture).
- **Quad derivatives.** `ddx_fine`/`ddy_fine` let the 4 pixels of a 2×2 quad exchange values within one pass.
- **Udon** (VRChat's scripting VM) for input, networking, URL loading and readback through `OnPostRender`/`ReadPixels`.

**Udon is faster than its reputation.** UdonSharp's docs once warned of 200–1000× slowdowns versus C#. The VRCnes world's own profiler, in the user's VRChat log, measured about 3.25 µs per emulated 6502 instruction in Udon, roughly 300k instructions per second. That runs on VRChat's main thread, though: VRCnes reports needing 866 ms of CPU per second at full speed. A shader core avoids that cost.

**Shader compiler fragility is a real risk.** pimaker reported FXC crashing with a generic "IPC error" on large shaders, whitespace changes deciding between crash and success, and compiles taking 10–15 minutes. Attributes matter: `[forcecase]` emits a jump table, `[flatten]` uses conditional moves, and `[loop]` stops unrolling, which is the only tool to avoid duplicate inlining.

## Target choice

Windows 3.0 in real mode is the best target: it is the most capable Windows that runs on the 8086 core that DOS already needs. The cost jump comes at the 386, not between DOS and Windows.

| Target | CPU core needed | Assessment |
| --- | --- | --- |
| MS-DOS | 8086 | First milestone; boots in minutes |
| Windows 1.0 / 2.x | 8086 | Same core as DOS; needs real devices, not just BIOS calls |
| **Windows 3.0, real mode** | **8086** | **Recommended.** Newest Windows on an 8086; VGA, EGA or 8514/A drivers; cramped in 640 KB without EMS |
| Windows 3.0 / 3.1, standard mode | 286 | Protected mode with descriptor tables, A20 gate, up to 16 MB, no paging, still 16-bit. A sensible next step |
| Windows 3.x enhanced mode, Windows 95 | 386 | Order-of-magnitude jump in core size and speed needs |

**Windows 3.0 compared with DOS.** The extra work is devices, not CPU. Windows ships drivers that touch hardware directly, so intercepting BIOS calls stops being enough: keyboard via port 60h and IRQ 1, a serial mouse, a correct interrupt controller and timer, a hard disk image. Estimated at 1.5–2× the emulator code of a minimal DOS machine.

**Why Windows 95 is out of reach**

- **CPU.** It needs a 386 with 32-bit operand and address prefixes, SIB addressing, the 0Fh opcode map, protected-mode segmentation with privilege checks, paging, virtual-8086 mode and the I/O permission bitmap. Expected 3–5× more cost per instruction, and a shader that may exceed what FXC tolerates.
- **Speed.** Very roughly 0.5–1 billion instructions to reach the desktop (no measured figure found). At 50–150k instructions per second, that is one to several hours.
- **Memory.** 8 MB of RAM (4 MB swaps constantly) is 524,288 texels, so the full-texture commit becomes 8× more expensive. A 50 MB disk image fits in a 2048² integer texture but inflates world download size.
- **Official requirements:** 386DX (386SX works slowly), 4 MB RAM minimum, 8 MB recommended, about 35–55 MB disk, VGA and mouse. No FPU required.

**Gaming ceiling, for reference.** At an optimistic 2 million instructions per second (roughly IBM AT speed), late-1980s titles run well: Sierra AGI adventures, Prince of Persia, SimCity, the EGA LucasArts adventures. Late-286 games are playable depending on the title. Wolfenstein 3D is borderline because it rewrites the whole VGA screen each frame. 386-only games such as Doom, and anything needing a 3D card, are out.

## CPU: the 8086 core

The whole 8086/8088 instruction set is needed; there is no smaller useful subset, because DOS, its tools and compiler output use a broad mix. No 286 or later features are needed for this target.

**Registers: 14, all 16-bit**

- General purpose AX, BX, CX, DX, each split into 8-bit halves (AH/AL and so on).
- Index and pointer SI, DI, BP, SP.
- Segment CS, DS, ES, SS, plus IP.
- FLAGS with 9 meaningful bits: CF, PF, AF, ZF, SF, OF (status) and TF, IF, DF (control).

Addresses are `segment × 16 + offset`, a 20-bit (1 MB) space that wraps at 1 MB; some programs rely on the wrap.

**Instructions: about 90 mnemonics, about 230 of the 256 primary opcodes**

- Data movement: MOV, PUSH, POP, XCHG, LEA, LDS, LES, XLAT, LAHF, SAHF, PUSHF, POPF, IN, OUT.
- Arithmetic: ADD, ADC, SUB, SBB, INC, DEC, NEG, CMP, MUL, IMUL, DIV, IDIV, CBW, CWD, and BCD adjusts DAA, DAS, AAA, AAS, AAM, AAD.
- Logic AND, OR, XOR, NOT, TEST; shifts and rotates by 1 or CL.
- String ops MOVS, CMPS, SCAS, LODS, STOS with REP/REPE/REPNE.
- Control flow: JMP (short, near, far, indirect), 16 conditional jumps, LOOP/LOOPE/LOOPNE, JCXZ, CALL, RET/RETF with stack adjust, INT, INTO, IRET.
- Flags CLC, STC, CMC, CLD, STD, CLI, STI; misc HLT, NOP, WAIT, LOCK, segment overrides, ESC. With no FPU, ESC is a no-op and the BIOS equipment word reports no coprocessor.

**Decoding is the real work.** Most instructions use a ModR/M byte selecting a register or one of 8 memory forms (`[BX+SI]`, `[BX+DI]`, `[BP+SI]`, `[BP+DI]`, `[SI]`, `[DI]`, `[BP]` or direct, `[BX]`) with an 8- or 16-bit displacement. BP-based forms default to SS. Groups 80h–83h, D0h–D3h, F6h/F7h and FEh/FFh use ModR/M's reg field as a sub-opcode. Instructions are 1–6 bytes plus prefixes.

**Quirks to emulate**

- Offsets wrap within a 64 KB segment: a word read at FFFFh takes its high byte from 0000h.
- `MOV SS` / `POP SS` block interrupts for one instruction.
- Divide errors raise INT 0, and on the 8086 the saved return address points after the DIV.
- CPU-detection probes: `PUSH SP` pushes the decremented value, shift counts are not masked, FLAGS bits 12–15 read as 1.
- Undocumented aliases: 60h–6Fh act as 70h–7Fh, C0h/C1h as C2h/C3h, C8h/C9h as CAh/CBh, D6h is SALC.
- An interrupted REP string instruction resumes correctly only with a single prefix; emulating the multi-prefix bug is optional.
- TF raises INT 1 after each instruction (used by DEBUG.COM). Flags documented as undefined after MUL/DIV rarely matter.

**Boot:** the CPU starts at FFFF:0000; the BIOS loads the boot sector to 0000:7C00 and jumps to it.

For scale, 8086tiny boots DOS with a complete 8086 and PC hardware emulator in about 4 KB of C.

## Machine and devices

Windows 3.0 needs real port-level devices because its drivers bypass the BIOS. Each is a small, well-documented chip.

| Device | Purpose | Notes for this project |
| --- | --- | --- |
| 8259 interrupt controller | Routes hardware IRQs | Windows reprograms and hooks it, so it must be correct. The AT's second, cascaded 8259 is only needed for a 286/386 machine |
| 8253 timer (PIT) | INT 8 at about 18.2 Hz; PC speaker | Fixed 1.193182 MHz input. Derived from instruction count, guest time drifts from real time |
| Keyboard controller | Scancodes on port 60h, IRQ 1 | Windows hooks INT 9 directly. Input comes from Udon through a uniform |
| 8250 UART on COM1 | Microsoft serial mouse, IRQ 4, 3-byte packets | Mouse deltas and buttons from Udon through uniforms |
| VGA | Text mode, BIOS screens, DOS | Required alongside an 8514/A, which was a companion card. Minimal text mode plus INT 10h is enough |
| Hard disk | DOS 5 + Windows 3.0 in 10–20 MB | A disk-image texture. Handle INT 13h in the emulator rather than emulating a controller |
| LIM EMS board | Expanded memory for real-mode Windows | Optional but likely worthwhile: 16 KB pages of a large pool mapped into a 64 KB window (D000 or E000) by port writes. One page-table lookup in address translation |

**BIOS options**

- **Emulated (high-level).** Intercept INT 10h, 13h, 16h, 1Ah, 11h/12h and 19h in the emulator. Simplest, and lets the power-on memory test be skipped (it costs millions of instructions).
- **Real ROM.** The open-source GLaBIOS, once the chips above are emulated correctly.

**Disk reads should bypass the CPU's write path.** Windows loads around a megabyte of code, fonts and resources at startup. Route INT 13h sector reads through the bulk-copy path in the commit pass (see Shader architecture), or they will dominate boot time. Disk writes need the same commit mechanism as RAM; VRChat has no persistence, so the disk can reset on each visit.

**Display decoding is already parallel.** A display shader reads the framebuffer region of the RAM texture (B800:0000 for color text, or the graphics planes) and renders it through a font atlas and palette, one screen pixel per fragment.

## Graphics: the 8514/A accelerator

An emulated 8514/A turns Windows' drawing into parallel shader work and takes screen traffic off the CPU's write path, which is the project's main bottleneck.

**The problem it solves.** CGA, EGA and VGA were dumb framebuffers: every line, fill, glyph and window move was the CPU writing bytes, one at a time. A GUI repaint is far more write traffic than DOS text mode's 2 bytes per character. A full CGA repaint alone is 16 KB, or 1024 texels.

**How offload works**

1. Windows' display driver sends a command, such as "BitBlt rectangle A to rectangle B with raster op X", through a few I/O port writes.
2. The emulated CPU stalls on it until the next pass, like rvc stalling on `fence.i`.
3. In a full-texture pass, every texel in the destination rectangle computes its own new value: read the source, apply the raster op, write. One pass, all pixels at once.

This matches GDI's design (BitBlt with raster-op codes, plus lines, fills and text), which display drivers were free to send to hardware. pimaker was prototyping the same idea in rvc: a guest-triggered memcpy executed in the commit pass.

**Video memory outside the 1 MB.** The real 8514/A's memory was reachable only through I/O ports. Mirror that: a separate texture, for example 1024×768 at 8 bits per pixel (768 KB) in a 1024² R8 or packed layout. Windows then almost never writes pixels through the CPU, fonts and icons are cached off-screen once, and text becomes small blits.

**Details and catches**

- **Driver.** Windows 3.0 ships an 8514/A driver, so emulating the documented register interface lets the stock driver work. Unverified: whether that driver runs in real mode. If not, a custom display driver is needed (Windows 1.0 predates the 8514/A in any case).
- **Pixel-transfer port.** The CPU streams bitmaps and fonts through a port. That is serial, write-only traffic and needs its own queue; it is mostly front-loaded because fonts are cached.
- **Batching.** One pass per command is too slow when text issues hundreds of tiny blits. Queue N commands and loop through them per texel. Split a batch whenever a command reads a region an earlier one in the batch writes, since that texel reads last frame's texture.
- **Pixel format.** Unaligned copies in a 1-bit CGA-style packing need shifts across 2–3 source texels; a linear 8bpp off-screen format avoids this.
- **Without an accelerator,** start with CGA 640×200 mono (a 16 KB interleaved framebuffer at B800). EGA's planes, latches, write modes and masks are branchy code on the hottest path.

## Shader architecture

The core design is rvc's tick/commit loop, adapted to a 1 MB machine: the CPU runs many instructions per pass in one pixel's registers, buffers its memory writes, and a commit pass applies them to the RAM texture.

&#91;embedded content: per-frame passes · CPU, commit, 8514/A, display\]

Only the CPU tick is serial; commits, blits and display decoding each run one pixel per texel. Udon feeds keyboard, mouse and URL loads in as uniforms.

### Memory layout

- **RAM:** 1 MB = a 256×256 RGBA32UI texture (65,536 texels × 16 bytes). That is the size of rvc's state area alone, so it fits in GPU L2 and a full commit is nearly free. rvc needed a 2048² texture for 64 MB.
- **CPU state, write log and queues:** a small separate state region.
- **Separate textures:** 8514/A video memory, the EMS pool, the disk image.
- **Byte granularity:** one texel holds 16 bytes, so byte and word writes are read-modify-write. An R8\_UINT layout avoids that at the cost of more reads per access.

### Tick and commit

rvc's evolution: version 1 ran one instruction per frame over the whole texture; version 2 ran ticks on a 128×128 state area, then committed; version 3 looped thousands of ticks inside one pixel, reaching 250k instructions per second on a 2080 Ti. With 1 MB instead of 64 MB, this project can alternate tick and commit passes dozens of times per frame.

In rvc's final source, the Custom Render Texture (2048×4096, RGBA32 unsigned integer, double-buffered, updated every frame) has two update zones. The 64×64 state area runs the CPUTick pass (default 1024 ticks divided by 4), then the whole texture runs the Commit pass, where every pixel decodes the full CPU state. Pixels outside the state area pass through unchanged in the tick pass.

### The write problem

Writes must be visible at once (a PUSH followed by a POP must see the value), but a fragment shader only writes at the end of a pass. **rvc's final source solves this with a hashed, set-associative write cache in an indexable array**, not the 320-byte register cache its 2021 blog post describes.

- `static uint4 l1_cache[1024]` in the tick pass, each entry holding two (address, value) pairs: 2048 cached words, 8 KB. A dynamically indexed array compiles to an indexable temp, which FXC accepts.
- The index is a hash of the address (`RAM_L1_ARRAY_IDX`: low word bits plus two higher bits) into 512 sets, in two slices, so each address has 4 possible slots.
- A one-word bloom filter (`mem_cache_bloom |= addr`) skips lookups for addresses not written this pass, and writes that do not change the value are dropped.
- When all 4 slots are taken, the CPU stalls (`STALL_MEM_CACHE_L1`) until the next commit.
- `encode()` writes the array into a fixed region of the state texture. In the commit pass, **each RAM texel gathers its own updates**: per word it hashes the address and checks both slices, 8 state-texture taps per texel. No scatter is needed.

This gather design is proven and simple, so it is the recommended baseline. For a 1 MB machine the commit covers 65,536 texels, versus about 8.4 million in rvc.

If the hashed cache stalls too often (Windows redraws and boot-time loading are the likely triggers), these unproven ideas from the design discussion add capacity:

1. **Partitioned write log, no communication.** Every pixel runs the same CPU redundantly and holds one slot: `if (write_count == my_slot) { kept_addr = addr; kept_val = val; } write_count++;`. One compare and two registers per pixel, so a 4096-entry log is 4096 pixels. Each copy is real GPU work, so profile per-copy speed as slots grow.
2. **Small replicated forwarding cache** (last \~16–32 writes) in every pixel, for read-after-write.
3. **Dirty bitmap** (a few hundred bits, hashed by address) catches the gap. On a read: cache hit → use it; bit clear → texture is valid; bit set but no hit → end the pass early and commit. All copies hold identical state, so they agree on when to stop.

**Committing a partitioned log** would need a geometry-shader scatter: each entry becomes a point drawn at its texel, rasterized in submission order so later writes win. Integer targets cannot blend, so partial writes must be merged into whole texels at log time.

Write-only streams fit the same partitioned pattern with no detection needed: the 8514/A command queue, the pixel-transfer stream, UART output, and REP offload descriptors.

### Quad derivatives: four times the register file

`ddx_fine`/`ddy_fine` exchange values within a 2×2 quad (`neighbor = v ± ddx_fine(v)`). They work on floats, but integers up to 2²⁴ survive exactly, so convert numerically, never with `asfloat`. They require uniform control flow, which redundant execution guarantees. Each lane can hold a quarter of the cache sets and broadcast hits, quadrupling cache capacity without raising per-lane register pressure. Test FXC's acceptance and cross-vendor exactness on a prototype first.

### REP string offload

On a large REP MOVS or STOS, the CPU records a copy or fill descriptor and stalls; the commit pass executes it in parallel, one texel per pixel. This needs no guest changes and removes the worst cache floods. Overlapping copies (such as MOVSB with source one byte behind destination, a pattern-replication trick) and short counts must fall back to serial execution.

rvc already ships this pattern for memcpy. The guest writes source, destination and length to custom CSRs 0x0B1–0x0B3, and a write to 0x0B0 sets `STALL_MEMOP_COPY`. The commit pass then fills each destination texel from the source, reading through the write cache or even the ROM texture. Only the start addresses are translated, so ranges must be physically contiguous, which real-mode x86 always is.

### What does not work

- **Superscalar across pixels.** Pixels cannot share results within a pass, and lanes running different instructions diverge and serialize. x86 also has little exploitable parallelism: nearly every instruction touches flags, and instruction boundaries are only known after decoding.
- **Lookup textures for ALU ops.** A texture tap costs hundreds of cycles of unhidden latency in serial code; a shift or `countbits()` costs a handful.

### Per-instruction speedups

- **Decode tables** as `static const uint` arrays (an immediate constant buffer, not a texture): per opcode, ModR/M presence, width, immediate size, direction, ALU class, memory read/write. Similar tables for ModR/M modes and 8-bit register mapping. They also cut code size, which matters for FXC.
- **Lazy flags:** store the last operation and result; compute PF, AF and OF only when read. Parity is `countbits(x & 0xFF) & 1`.
- **A 16-byte prefetch window** (at most two texels per instruction) instead of one tap per byte.
- **One call site each** for memory read, memory write and effective address, since HLSL inlines every call. Use `[loop]` to share code.
- **Fast paths** for MOV, PUSH/POP, conditional jumps and register-register ALU ops; one REP iteration per tick if not offloaded.
- **Self-modifying code is common on x86**, so instruction fetch must check the write cache. rvc could skip that because RISC-V requires `fence.i`.

### Branch-related speedups

Classic branch prediction gains nothing here: the interpreter finishes each guest instruction before starting the next, so there is no queued work to save. The GPU has no branch predictor either, and redundant lanes never diverge on guest branches. Avoiding repeated work does pay off, in four ways.

**Parallel predecode texture (most promising).** x86 decoding is the costly part of each instruction. A separate texture can hold a decode for every byte offset of the 1 MB, computed as if an instruction began there:

- Each entry records instruction length, prefix count, ModR/M presence, operand class or decode-table index, and immediate/displacement offset.
- Every offset is independent, so it is a one-pixel-per-entry pass. Real decoders mark instruction boundaries the same way.
- The commit pass recomputes only entries in texels it rewrote; neighbors matter only for instructions crossing a texel edge (at most 6 bytes plus prefixes).
- In the CPU loop, decode becomes one texture read. If the bloom filter or write cache shows a written byte in the instruction's range, fall back to the slow decoder. That handles self-modifying code without extra machinery.
- Prefix chains and 0Fh-style escapes are rare on the 8086; store a "use slow path" flag for anything unusual.

**Interrupt and timer checks at branches only.** rvc checks the timer (with double-precision math), the UART and pending traps after every instruction. Checking only at taken branches, or every N instructions, removes that cost from straight-line code. Interrupts arrive a few instructions late, which DOS and Windows tolerate. Keep the one-instruction shadow after `MOV SS`/`POP SS` and the trap flag exact.

**Idle-loop elision.** Detect a short backward jump that polls a memory location or port without writing anything, and skip to the next event (timer tick, keyboard IRQ, retrace change). VRCnes does this about 23 times per frame and it is a large part of how it reaches full speed. DOS and Windows poll the keyboard, timer and video retrace constantly.

**Fused branch pairs.** Handle `CMP` + `Jcc`, `TEST` + `Jcc`, `DEC CX` + `JNZ` and `LOOP` as single fast paths, so flags are never materialized when the jump consumes them immediately. Lazy flags already cover most of this; fusion also saves a dispatch.

**Does not work:** running both outcomes of a branch speculatively on different quad lanes. After the branch the lanes execute different code, the wave splits, and both paths are paid for.

### Other details from rvc's source

These are from the [rvc repository](https://github.com/pimaker/rvc) as of its last commit (November 2022), mostly `_Nix/rvc/src/*.h.pp` and `main.shader`.

| Area | What rvc does | Use here |
| --- | --- | --- |
| Code generation | perlpp generates struct layout, `encode()`/`decode()` switch tables, CSR and cache offsets from one config file (`header.p`) | Same approach, or a Python generator |
| Memory map | RAM at 0x80000000 below a 64-row state band; ROM (initramfs) at 0x40000000 and device tree at 0x1020 in separate read-only textures; MMIO (CLINT, UART, RTC) in a `[forcecase]` switch | BIOS ROM, disk image and option ROMs as read-only textures; port I/O in a switch |
| Program images | Four PNGs per payload, one per 32-bit lane; each RGBA8 pixel is one word, rebuilt by `unpack_raw_float4`; the import preset disables sRGB, compression and filtering | Ship DOS, GEM and disk images the same way |
| Snapshots | An editor-only script renders the whole state to a 6144×8192 PNG (128 bits per texel over six RGB8 pixels); `_InitRaw` restores it, so the world can start from pre-booted Linux | Ship a pre-booted DOS + GEM snapshot to skip boot; a Windows snapshot would contain Windows itself |
| CSRs | 4096 registers in a texture region; writes go through a 16-entry register cache flushed at commit, stalling when full | Pattern for device registers that change rarely |
| Stalls | Codes for CSR cache, L1, `fence.i`, memop copy and UART; the commit pass clears them | Same mechanism for 8514/A commands and REP offload |
| Instruction fetch | Reads the RAM texture directly, bypassing L1; `fence.i` stalls to commit | Not possible on x86: fetch must check the cache |
| Decode and registers | Nine masked switches; only the large ones use `[forcecase]`, because using it everywhere "breaks compilation very badly"; registers are a real `xreg[32]` array, written via flattened switches | Expect the same per-switch tuning |
| Timer | `mtime` derived from Unity's `_Time` (wall clock), not instruction count | Same choice for the PIT keeps guest time real |
| Terminal | A separate 81×25 CRT replays the 64-entry UART ring buffer every frame in each cell, handling backspace, CR/LF, tab, a few ANSI cursor codes and scrolling | Precedent for log-replay rendering |
| Control | Udon switches the CRT between on-demand and realtime and shows instructions per second as ticks × updates per second | Same |

A 2×2 multi-hart layout (one CPU per pixel of a quad) is present but commented out.

## Performance expectations

At rvc-level speed, Windows 3.0 would boot in roughly 1–4 minutes; an optimized core might reach IBM AT speed. All figures below are estimates unless marked.

| Machine or emulator | Approx. instructions per second |
| --- | --- |
| IBM PC/XT, 8088 at 4.77 MHz | \~0.3 million |
| rvc on a 2080 Ti (measured by pimaker) | up to 0.25 million |
| VRCnes 6502 core in Udon (measured, user's log) | \~0.3 million |
| IBM AT, 286 at 8–12 MHz | \~1–2.5 million |
| This project, optimistic, RTX 5090 | \~2 million |
| 386DX-33 | \~8–10 million |
| 486DX-33 | \~25 million |

**Reaching 2 million per second.** rvc works out to about 7,000 GPU cycles per instruction; 2 million on a \~2.6 GHz 5090 needs about 1,300. A 5× efficiency gain looks achievable with the techniques above. Against it: shaders cannot JIT, so this stays an interpreter, and in a live VR world at 90 Hz the emulator shares the GPU, so usable speed may be 2–5× below a benchmark. A 386 core would likely run at about half the 8086 core's speed.

**Boot cost (no published figure found).** From power-on to Program Manager, real-mode Windows 3.0 is estimated at 10–50 million instructions including DOS (very roughly 100–700 million 8088 cycles). On real hardware it took around a minute, much of it disk I/O at \~100 KB/s. Emulated disk reads are instant, but loading \~1 MB into RAM is write traffic, hence routing it through the bulk-copy path.

**Cycles do not matter; instructions do.** The emulator spends roughly constant work per instruction, so MUL/DIV-heavy code runs relatively faster than on an 8086 and write-heavy code relatively slower.

**8086 clocks for reference:** the 8086 launched at 5 MHz (1978), with 8 and 10 MHz versions later; the IBM PC's 8088 ran at 4.77 MHz, turbo XT clones at 8–10 MHz. Instructions took 2 cycles (register MOV) to over 150 (DIV).

Measure the real instruction count by booting the same disk image in the C reference core or an instrumented emulator such as MartyPC.

## Development workflow with agents

The project suits parallel LLM agents because the spec is fixed and correctness is mechanically checkable; the infrastructure below is what makes that possible.

**Oracles**

- **CPU:** the SingleStepTests 8088 suite (per-opcode JSON before/after states). 286 and 386 sets are believed to exist too.
- **Whole system:** lockstep trace diffing against MartyPC, 86Box or Bochs; the first diverging instruction points at the bug.
- **Devices:** port-level behavior compared against 86Box or MAME.
- **8514/A and Windows:** screenshot diffs at known boot points.

**Infrastructure**

1. **One source, two targets.** Write the core once in the C/HLSL common subset, with macros smoothing differences, so it compiles both as C and as the shader. pimaker kept separate versions and ping-ponged; a shared source stops drift.
2. **Headless D3D11 harness outside Unity.** It compiles with FXC at Shader Model 5 (matching Unity's DX11 path, not DXC), runs tick and commit passes, reads state back and diffs it against the C core.
3. **WARP** (Microsoft's software D3D11 rasterizer) for correctness runs: no GPU needed, so many agents run in parallel. Its speed means nothing; tune on real GPUs.
4. **Partial builds.** Test shaders containing one opcode group or device compile quickly; run the full monolithic build periodically.
5. **Modules along clean interfaces** (CPU, decode tables, each device, 8514/A, memory and cache, harness) to avoid merge conflicts, with generated glue.

**Good agent work:** grinding trace diffs; bisecting FXC crashes (`[forcecase]` vs `[branch]`, splitting functions, loop restructuring); autotuning cache sizes, table layouts and switch attributes against instructions per second.

**Needs a human or real hardware:** GPU-vendor behavior (quad derivatives on NVIDIA, AMD and Intel); real performance; the VRChat layer (CRTs, camera loops, Udon input, world size limits) in the client.

**Guard the oracle.** Keep test data read-only and hold back a hidden test set, since agents under pressure may special-case tests or edit expected results.

**Local machine note.** The linked PC gives Claude a Linux shell in an isolated VM with the whole C: drive mounted. It can build and test the C core there, but cannot run Windows programs: no FXC, Unity or VRChat. Those need the user, a Windows build machine, or slower computer-use control. A dedicated project folder is a tidier connection than the whole drive.

## Content and licensing

Ship only openly licensed software inside the world, and load anything else from URLs users paste themselves. This is not legal advice.

**The principle.** Encoding a program as an image does not change its legal status; whoever hosts it is distributing it. Bundling a commercial program in a world distributes it to every visitor and breaks VRChat's rules on uploading content you lack rights to. A neutral "paste your own URL" tool sits closer to an emulator author's position (emulators are legal in the US after the Connectix and Bleem cases). "Free to download" is not "free to redistribute": look for an explicit license or written permission.

| Content | License | Role in the world |
| --- | --- | --- |
| [MS-DOS 1.25, 2.0, 4.0](https://github.com/microsoft/MS-DOS) | MIT (Microsoft) | Bootable genuine DOS |
| FreeDOS | GPL | Alternative DOS |
| GEM (OpenGEM / FreeGEM) | GPL (Caldera, 1999) | A legally shippable 1985 GUI that runs on an 8088; a stand-in for Windows |
| [Zork I, II, III](https://opensource.microsoft.com/blog/2025/11/20/preserving-code-that-shaped-generations-zork-i-ii-and-iii-go-open-source/) | MIT (Microsoft, Nov 2025); code only, not trademarks | Compile with ZILF, run in an open-source Z-machine such as Frotz |
| GW-BASIC | MIT (Microsoft, 2020) | Boot-to-BASIC demo; building from the assembly source takes work |
| ELKS | Open source | Linux-like system for the 8086 |
| Minix 2 | BSD (since 2000) | Ran on the 8088; inspired Linux |
| Moria, early Hack builds | GPL / open | Roguelikes; check 8088 compatibility |
| Windows 1.0, 2.x, 3.0 | Proprietary | Cannot ship; user-supplied only |

Shareware episodes (for example Commander Keen 1) usually require unmodified, free distribution, and rights holders have changed; check each title. The 8088 MPH demo is freely distributed but needs cycle-exact CPU and CGA timing, so treat it as a stretch accuracy test.

**URL loading in VRChat**

- **Image loader:** at most 2048×2048 (about 16 MB at 4 bytes per pixel), one download every 5 seconds across the whole world, direct links only (redirects fail), domains outside the allowlist need the visitor's "Allow Untrusted URLs" setting. Use PNG, avoid color-space conversion, premultiplied alpha, mipmaps and compression, and store a checksum.
- **String loader:** the VRCnes world loads binary ROMs this way, parsing 512 KB successfully. It shares the 5-second limit and the no-redirect rule (archive.org links failed with "Redirect limit exceeded").

**Disk images.** Preinstall Windows into the disk image with 86Box or similar on a PC; running Setup inside VRChat would take far too long.

**Multiplayer.** Integer-only emulation is deterministic, so lockstep multiplayer is possible by syncing inputs only. Late joiners need a state snapshot, which is about 1 MB for this PC and hard to transfer.

## Prior art

rvc proves an OS can boot in a VRChat pixel shader, and two NES worlds prove full-speed console emulation; no x86 PC emulator in VRChat was found.

| Project | Approach | What it shows |
| --- | --- | --- |
| [rvc](https://github.com/pimaker/rvc) by pimaker ([blog](https://blog.pimaker.at/texts/rvc1/)) | RV32IMA + SV32 MMU in an HLSL pixel shader; one 2048×4096 RGBA32 integer texture (64×64 state area, RAM below); Linux 5.13.5, OpenSBI, Micropython | Up to 250k instructions/s on a 2080 Ti; tick/commit with a hashed 8 KB write cache and gather commit; memcpy offload; snapshot boot; FXC pain points |
| VRCnes emulator \[Full speed\] (`wrld_e6f7262b…`) | 6502 interpreted in Udon; per-scanline PPU latch; ROMs via string downloader | Full NES speed from Udon by skipping idle loops; at a cost of 866 ms of main-thread CPU per second |
| \[Local\] NES Emulator in VRC – Just Famicom (`wrld_9d1e72b6…`) | Unknown core behind a mailbox interface (magic "VNE", API 1) | Little else visible in logs |
| [VRC-CDP1802](https://github.com/89Mods/VRC-CDP1802) | CDP1802 8-bit CPU in a Unity shader | Works on avatars and in worlds |

**From VRCnes's own profiler (user's VRChat log, 5 October 2026)**

- About 3.25 µs per instruction for LDA #imm, 3.69 µs for the last switch case, 2.26 µs for NOP.
- Per NES frame: CPU 13.31 ms (92%), overhead 1.10 ms (per-scanline latch, sprite 0, loop), total 14.40 ms, "max 69 NES fps if nothing else ran".
- Idle-loop elision fired about 23 times per frame; flags `skipIdleLoops`, `holdSpinAcrossScanlines`, `fastScanlineLatch`, `useAbs16Table`.
- On load it pre-decodes CHR (tile) data, about 2 s for 256 KB, presumably into a texture.
- ROMs loaded: AccuracyCoin (accuracy test), a Flappy Bird homebrew, FC-WIN98 (an unlicensed Famicom cart, believed to imitate Windows 98). Host nono.rip is not on VRChat's allowlist.
- Neither world's log says anything about audio.

**Lessons for this project.** Idle-loop elision is worth copying: DOS and Windows spend much time polling the keyboard and timer. Udon is a viable fallback for slow peripherals, but the CPU core belongs in a shader so it does not compete with VRChat's main thread.

The NES itself is a much easier target (about 0.5 million instructions per second for full speed, about 12 KB of writable memory, a parallel-friendly graphics chip) and is already solved in VRChat, so it is not pursued here.

## Build order, risks and open questions

Build the C reference core and its test harness first; every later stage is checked against it.

**Build order**

1. **C 8086 core**, validated against SingleStepTests, written in the C/HLSL common subset.
2. **Headless D3D11 harness** (FXC, SM5, WARP) diffing shader state against the C core.
3. **HLSL port** with tick/commit, the 256² RAM texture, a hashed write cache and text-mode display, starting from rvc's \`\_Nix\` folder as a template. Milestone: DOS boots to a prompt.
4. **Timer, interrupt controller, keyboard, serial mouse, disk image.** Milestone: Windows 1.0 or 3.0 real mode starts with a stock CGA or VGA driver.
5. **Write-path upgrades:** rvc-style hashed write cache with gather commit (from step 3), then REP and disk-read offload, idle-loop elision, the predecode texture and branch-only interrupt checks; the partitioned log and dirty bitmap only if stalls remain.
6. **8514/A:** command queue, batched blits in the commit pass, separate video-memory texture. Milestone: Windows 3.0 at 1024×768 with responsive redraws.
7. **Unity/VRChat integration:** CRT setup, Udon input and URL loading.
8. **Optional:** EMS, disk writes, PC speaker, quad-distributed cache, lockstep multiplayer.

**Risks**

- **FXC.** 8086 decode plus devices plus blit logic in one shader may hit crashes and 10-minute compiles. Keep the 8514/A in a separate shader or pass.
- **Write bandwidth.** If the log and offloads underperform, GUI redraws and boot slow badly.
- **Batch hazards** in 8514/A command batches that read what an earlier command wrote.
- **Self-modifying code** forcing instruction fetch through the write cache on the hottest path.
- **Frame budget** in a live VR world, possibly 2–5× below benchmark speed.
- **Quad-derivative tricks** may be rejected by FXC or behave differently across GPU vendors.

**Open questions**

- Does Windows 3.0's stock 8514/A driver work in real mode?
- What is the measured instruction count for a real-mode Windows 3.0 boot?
- How many write-log slots can run before per-copy speed drops?
- Does Udon support efficient raw audio output for the PC speaker?
- How does the Just Famicom core work, and do either NES world's authors publish code?
- Is a 286 core (Windows 3.1 standard mode) worth it after the 8086 milestone?

## Sources

- [Linux in a Pixel Shader – A RISC-V Emulator for VRChat](https://blog.pimaker.at/texts/rvc1/) (pimaker, 2021)
- [rvc on GitHub](https://github.com/pimaker/rvc)
- [VRC-CDP1802 on GitHub](https://github.com/89Mods/VRC-CDP1802)
- [Image Loading – VRChat Creator Docs](https://creators.vrchat.com/worlds/udon/image-loading/)
- [Trusted URLs – VRChat Wiki](https://wiki.vrchat.com/wiki/Trusted_URLs)
- [External URLs – VRChat Creator Docs](https://creators.vrchat.com/worlds/udon/external-urls/)
- [Microsoft Open Source Blog: Zork I, II and III go open source](https://opensource.microsoft.com/blog/2025/11/20/preserving-code-that-shaped-generations-zork-i-ii-and-iii-go-open-source/)
- [DirectX – BetaWiki](https://betawiki.net/wiki/DirectX) (DirectX 8.0a was the last for Windows 95)
- User's VRChat output log, 5 October 2026 (VRCnes profiler output, world IDs)

Figures without a source above (instruction counts, boot estimates, speed targets) are estimates from the design discussion and should be measured.
