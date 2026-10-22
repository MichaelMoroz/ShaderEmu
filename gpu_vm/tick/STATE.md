# The tick's state, its passes and its draws (step x17 on)

What a harness needs to run gpu_vm's tick (`gpu_vm/tick/steps/x17.hlsl`): the textures, the cbuffers, the draws of a pass
in order, and where a core's state lies. The semantics are the reference's (`gpu_vm/ref/gvm_ref.h`); `gvmbench.cpp` is a
working harness (`Gpu::run_pass`). Every texture is `R32G32B32A32_UINT`. The constants named here are the step's
(`static const uint` in its source): `WCN` 130, `RAM_W` 8192, `T_WC` 40, `STATE_TEXELS` 105, `OP_SLOTS` 32, `OP_SPAN` 272.

## Textures

| Slot | Name | Size | What |
|---|---|---|---|
| t0 | `_Ram` | 8192 x RamRows | RAM from address 0: byte address a is texel a >> 4 at (t % 8192, t / 8192), word (a >> 2) & 3. The boot area (0-0xffff, the device tree at 0x1000), the devices (0x10000-0x1ffffff) and RAM proper (0x2000000 to RamEnd) are all texels of it. Rows past RamEnd's must be zeros (a fetch there is path 0: a fetch fault). |
| t1 | `_Rom` | 8192 x rows | the ROM (0x40000000 on): offset o is texel o >> 4 at the same place. Read only; past its rows reads 0. |
| t2-t9 | `_S0`-`_S7` | StatePix x cores | a core's state, one row a core (core c is row c): state texel T is texture `T & 7`, column `T >> 3`. StatePix = (STATE_TEXELS + 7) / 8 = 14. Two sets, ping-pong: the tick reads one set and draws the other. |
| t10 | `_OpState` | cores x 1 | the operations' progress, a texel a core; two, ping-pong, a round each (below). Start them at zeros. |
| t11 | `_Stage` | OP_SPAN x (cores * OP_SLOTS) | the operations' stage: a row a slot (core c's slot q is row c * OP_SLOTS + q). Contents need no start. |

RamEnd must be a multiple of 16 and at most 0x20000000 (the fast loop takes `a & ~0x40000000 < RamEnd` for "RAM or ROM").

## A core's state texels

| T | x | y | z | w |
|---|---|---|---|---|
| 0-15 | r[4T] | r[4T+1] | r[4T+2] | r[4T+3] (r0 is written 0) |
| 16 | pc (bits 3:2: the word a resumed bundle goes on at) | STATUS | EPC | CAUSE |
| 17 | TVAL | TVEC | SCRATCH | IEN |
| 18 | TCMP_LO | TCMP_HI | flags | RANDOM's state |
| 19 | CYCLE (bundles run, low 32 bits) | OPS (ops run, low 32 bits) | bundles run in the last pass | why the last pass ended |
| 20 | latched operation word 0 | 1 | 2 | 3 (`GVM_OPS + 0..12` as stored) |
| 21 | latched word 4 | 5 | 6 | 7 (`GVM_OPS + 16..28`: a blit's second texel) |
| 22 | operations in flight (0-8) | segments of the first one already made | the queue's generation | operations refused, ever |
| 23 | 0 | | | |
| 24-39 | the queue: operation k is texels 24 + 2k (kind, dst, src, bytes) and 25 + 2k (dst stride, src stride, bytes a row, rows), in the order asked | | | |
| 40-104 | the write cache: texel 40 + i holds entries 2i (x word address >> 2, y value) and 2i + 1 (z, w); a word of 0xffffffff is an empty entry. Written by the tick, read by the commit. | | | |

Flags (18.z): 1 waiting (a supervisor's `wait` until IPEND & IEN), 2 the software interrupt's pending bit (IPEND bit 0),
4 halted (`break` with `_Flags` bit 0: the bench's), 8 in a `sync` (the core goes on when no operation is in flight),
16 the pass is made again (the last one met a load or store its fast loop does not make: outside RAM and the ROM, a
store below the devices or to the operations; it left the state as it found it, and this one runs every memory bundle
in the general step).

Why a pass ended (19.w), as `GVM_END_*`: 0 its bundles ran out, 1 sync (or an access an operation in flight holds, or a
ninth operation), 2 wait, 3 the write cache full, 5 halted.

A new core: everything 0 but pc (16.x), STATUS = `GVM_ST_S` (16.y, 8: supervisor), TVEC (17.y), RANDOM's state (18.w,
the reference's `0x9e3779b9 ^ (id * 0x85ebca6b)`), and r5 = 0x1000 for a kernel's boot. A halted core runs nothing.

## The cbuffers

```
cbuffer PassParams : register(b0) {                 // 80 bytes, set once a pass
    uint _Ticks;          // bundles a pass, a core
    uint _RamEnd;         // bytes: RAM's end, from address 0
    uint _TimeLo, _TimeHi;   // TIME (100 kHz) for the whole pass; the timer interrupt is TCMP != 0 && TIME >= TCMP
    uint _Player;         // the PLAYER special register
    uint _Flags;          // bit 0: break halts the core (the bench); 0 for a machine
    uint _RamRows;        // _Ram's rows
    uint _Ext0;           // (unused since x17: x16's external line; kept so that x16 reads the same layout)
    uint _NCores;         // cores (rows) that run
    uint3 _pad;
    uint4 _Ext[2];        // the external interrupt lines: core c's is bit (c & 31) of _Ext[c >> 7][(c >> 5) & 3]
};
cbuffer CommitParams : register(b1) { uint _Comp; uint _Cores; uint2 _pad; };   // the commit: the component (0-3), cores
```

## A pass, its draws in order

1. **The tick** (`vert`, `frag`): a full-screen triangle (Draw(3), triangle list) into the 8 textures of state set
   `cur` (viewport StatePix x cores), reading set `cur ^ 1` (t2-t9), `_Ram` (t0), `_Rom` (t1) and `_OpState[oc]` (t10).
   Each core runs up to `_Ticks` bundles.
2. **The commit**, four draws (`commit_vert`, `commit_frag`): Draw(cores * WCN) points into `_Ram` (viewport 8192 x
   RamRows), reading set `cur`, CommitParams `_Comp` = 0, 1, 2, 3 in turn with a blend state that writes that component
   alone (`RenderTargetWriteMask` 1 << _Comp, no blending). Core 0's stores first, the highest core's last.
3. **The operations**, in rounds (the bench makes 2 a pass; more rounds make more of a long operation in a pass, and a
   round with nothing to do draws nothing). A round is three draws, then `oc ^= 1`:
   - stage (`op_stage_vert`, `op_stage_frag`): DrawInstanced(6, cores * OP_SLOTS), triangle list, into `_Stage`
     (viewport OP_SPAN x cores * OP_SLOTS), reading `_Ram`, `_Rom`, set `cur` and `_OpState[oc]`;
   - write (`op_write_vert`, `op_write_frag`): DrawInstanced(12, cores * OP_SLOTS), triangle list, into `_Ram`
     (viewport 8192 x RamRows), reading `_Stage` (t11), set `cur` and `_OpState[oc]`;
   - progress (`op_prog_vert`, `op_prog_frag`): Draw(cores) points into `_OpState[oc ^ 1]` (viewport cores x 1),
     reading set `cur` and `_OpState[oc]`.
4. `cur ^= 1`. The next pass's tick reads the set this one drew and `_OpState[oc]` (the last round's).

No texture is bound as a resource and a target at once: unbind between draws (`gvmbench`'s `clear_srv`).

An operation (a 16-byte store of its descriptor at `GVM_OPS`) joins the core's queue (up to 8 in flight; a ninth ends the
pass until one is through). A round makes the queue in order, up to OP_SLOTS pieces of 4 KB a core (a copy or a fill is
its destination's 4 KB pieces; a blit's row its pieces); a blit whose rows share texels or whose source and destination
overlap takes a piece a round. The progress texel (generation, operations through, segments of the next, refused) tells
the next tick which to drop from the queue. Stores of a pass land before its operations (the reference's order).

## Between passes: the host

- **A core's state**: read the 8 textures of the set just drawn (`cur ^ 1` after the swap) at row c; texel T is texture
  T & 7, column T >> 3. Registers 0-15, pc 16.x, flags 18.z, why the pass ended 19.w.
- **RAM**: write texels of `_Ram` (UpdateSubresource of a box, or a draw) between passes, after the commit and the
  operations. A core's own stores of the pass are in RAM by then; nothing is cached across passes (the write cache is
  empty at a pass's start). The console (`0x016b8280`, `gvm_cpu.c`) is plain RAM words both ways.
- **A core's state written**: set texels of the set the next tick reads (as `prepare()` in `cases.h` makes a core).
- **Interrupts**: the external line by `_Ext`; the timer by `_TimeLo/_TimeHi` (advance it every pass: 20 ticks of 100
  kHz is 0.2 ms); a core in a supervisor's wait (18.z & 1) runs nothing until one is pending
  and enabled, so a host may skip time forward when every core waits (as `gvm_cpu.c` does).
- **Halt and sync**: flags 18.z & 4 (halted), & 8 (waiting for its operations).
