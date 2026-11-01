# A large scratch register file in the tick, priced on the card (1 November 2026)

## What the driver made of it: the card's code on D3D12 (`sass.ps1`, `sass_step.py`, `sass_same.py`, `sass_win.py`)

**At every N the array is in local memory, one `LDL` and one `STL`, in the same code. The guess is wrong for this path, and this code does not explain why 32 costs seven times what 64 does.**

Which path: the D3D11 blob of each side, converted to DXIL and traced on D3D12, the only path with SASS. The times below this section are D3D11's driver, which compiles DXBC itself and whose code cannot be seen. So the guess is refuted for D3D12 and untested for D3D11, and that gap is where the answer may lie.

Measured, read from the SASS (each trace has the tick twice under one module hash, `s0` and `s2`; why twice was not looked into, and both have the same form):

| N | registers a thread, `s0` / `s2` | instructions, `s0` | constant-address `STL`, `s0` | `sc` in `l[]` | `xr` in `l[]` | lines from the fields to the first `BRA`, waits |
|---|---:|---:|---:|---|---|---|
| tree | 121 / 113 | 7,984 | not counted | none | `0x0` | 27 |
| 16 | 122 / 119 | 8,072 | 194 | `0x0` | `0x100` | 40 |
| 32 | 122 / 119 | 8,104 | 226 | `0x0` | `0x200` | 40, same opcodes and waits as 16 |
| 64 | 122 / 120 | 8,168 | 290 | `0x0` | `0x400` | 40, the same |
| 256 | 122 / 119 | 8,656 | 546 | `0x0` | `0x1000` | 40, the same |

No `BRX` serves the array: a listing has 4 at every N, all the tick's own. No indexed register form appears anywhere. The access at N = 32, `s0` (N = 64 differs only in `0xfc` and `0x400`, register numbers apart):

```
89a0  LOP3.LUT R76, R76, 0x7c, R7, 0x48, !PT     ?WAIT3_END_GROUP;   (index from w and pc)
89c0  IMAD.SHL.U32 R76, R76, 0x4, RZ             ?WAIT3_END_GROUP;   (x 16 bytes in all: an element is a texel)
89d0  IADD3 R85, PT, PT, RZ, 0x200, R82          ?trans2;            (xr[rs1], base moved by the array)
89e0  IADD3 R80, PT, PT, RZ, 0x0, R76            ?trans2;            (sc[ri])
8a00  LDL R113, [R85] &rd=0x0 &wr=0x4            ?trans4;
8a10  LDL R87, [R80] &rd=0x1 &wr=0x2             ?trans1;
8bc0  STL [R88], R89 &rd=0x0                     ?trans1;            (sc[wi])
```

The tree's `xr`, and its write cache `c` at `0x410`, take the same forms:

```
84e0  LDL R112, [R76] &wr=0x4                    ?trans4;            (xr[rs1])
11480 STL [R7], R74 &rd=0x3                      ?trans2;            (xr[dst])
8cc0  LDL R85, [R81+0x8] &wr=0x2                 ?trans4;            (a bucket's tags, 0x40 to a bucket)
9420  LDL R80, [R79] &rd=0x0 &wr=0x4             ?trans4;            (a texel: then +0x4, +0x8, +0xc)
a7b0  STL [R77], R90 &rd=0x2                     ?trans4;            (a texel: then +0x4, +0x8, +0xc)
```

Measured as well:
- From 16 to 32 and 32 to 64 the listings differ by 2N in constant-address `STL` (32, then 64) and a few other lines elsewhere (`IADD3`, `NOP`, `MOV`, net under 15). Each word of `sc` is stored twice before the loop; that the two are fxc2's entry `mov`s and the patch's loop, unrolled, is inferred. 64 to 256 adds 256 such `STL` and some 230 lines to make their addresses (`ULEA`, `UIADD3`).
- Registers do not follow the time: 122 at each N in `s0`, and the array's own cost is 1 register on `s0`, 6 or 7 on `s2`.

Question by question:
- `xr` against a word of `c` (0 to 2 ns against 5 to 20): **the SASS does not explain it.** In the tree both are an `LDL` of a base plus 16 times an index, one line for one word. Those probes' own tracings were not taken, so whether the driver dropped the probes' dead store to `xr[64]`, as that note feared, is still not known. A difference of latency between places in `l[]`, or of what hits what in a local cache, would not show in SASS at all: not measured.
- 32 against 64: **not explained.** The access, its waits and the zeroing's form are the same. What changes is only `xr` and `c` moving up with the array (`0x200` against `0x400`) and 64 more stores at entry, which would make 64 dearer, not cheaper.

Not known, and what could tell (not run, as this needed no timing):
- Whether D3D11's driver chooses otherwise below 64 words. This is now the leading candidate, inferred only from the D3D12 code being level. A D3D11 side with `sc[64]` declared and its index masked to 31 would part size from the place touched: if it costs what 64 costs, the driver's choice goes by the declared size.
- Whether `l[]`'s placement matters on either driver: the same array read at a different base, or `xr` before it.
- D3D12's own time at each N. `rvc_trace12` is known not to get on with the guest (`rv_fuse/RESULTS.md`), so it could not be read from these traces.

The listings are not kept (`build/sc_sass`, ignored). Remake: copy `rvc_lean` to `rvc_lean_sc`, apply the patch, `pwsh -NoProfile -File experiments\rv_scratch\sass.ps1 -Name n32 -Defs SC_N=32` (the tree: `-Name tree`), about 90 s a side. The traces were whole and none was killed.

## The times

The question: what does a local array of N words cost lean's fast loop, and at what size does it stop being free? **It is not free at any size tried.** The cheapest is 64 words, 13 ns an instruction (8%) over the tree. Arrays of 32 words or fewer cost more than larger ones.

RTX 5070 Laptop, D3D11, fxc2, `ab.py` in turn from one snapshot, 3 runs a side, gzip only. Median clock 2,835 MHz in every run; no run discarded. Samples as low as 885 MHz appear in a run's minimum: the sampler runs over the whole process, start included, and which of them fell inside the timed window was not told apart. Tree over two calls 168.8 to 170.0 ns; a side's runs agree within about 2 ns.

`lean_sc.patch` on a copy of `rvc_lean` named `rvc_lean_sc` (git ignores it): every fast-loop instruction reads a word and writes a word at indices from `w` and `pc`, and what it reads is XORed into texel 39.w, a real output.

| N | ns an instruction | ns more than the tree | x | tick blob, bytes |
|---|---|---:|---|---:|
| tree | 169.1 | | 1 | 180,456 |
| 0: indices and sink, no array | 170.7 | 1.6 | 0.991 | 180,672 |
| 16 | 215.7 | 46.6 | 0.784 | 181,280 |
| 32 | 256.6 | 87.5 | 0.659 | 181,664 |
| 64 | 181.9 | 12.8 | 0.930 | 182,432 |
| 128 | 190.8 | 21.7 | 0.886 | 183,968 |
| 256 | 199.8 | 30.7 | 0.846 | 187,040 |
| 512 | 207.8 | 38.7 | 0.814 | 193,184 |
| 1024 | 212.7 | 43.6 | 0.795 | 205,472 |
| 2048 | 215.2 | 46.1 | 0.786 | 230,048 |
| 64, `uint4` by selects | 195.7 | 26.6 | 0.864 | 182,008 |
| 256, `uint4` by selects | 226.9 | 57.8 | 0.745 | 183,736 |
| 2048, `uint4` by selects | 246.5 | 77.4 | 0.686 | 199,864 |

Compiled: `dcl_temps` 204 on the tree, 205 on every array read (N = 0, 32, 256, 2048). The fast loop's text is 4,657 instructions on the tree and 4,667 with an array of any size read, so size does not change the DXBC of the access. The blob grows 24 bytes a word: fxc2 writes each element of a static array at the shader's entry, N `mov`s (6,979 instructions at 32, 8,995 at 2048), on top of the patch's own loop.

Measured:
- The array's step lies between 32 and 64 words, and it runs the other way from what was expected: below it, dearer. N = 16 and 32 also rise with N, 47 then 87 ns.
- From 64 up the cost rises about 8 to 10 ns a doubling, flattening to 2.4 at the last.
- Words in a plain array are cheaper than `uint4` with word selects at every size compared (13 against 27 at 64, 31 against 58 at 256, 46 against 77 at 2048).
- The blob reaches 230 KB at 2048 words, nowhere near the 541 to 696 KB cliff. Note that `cso_loops.py`'s header says the tick is over 500 KB; here it is 180 KB, and why they differ was not looked into.

Inferred, not run:
- Up to 32 words the driver keeps the array in registers and reaches them by a dynamic jump or a walk, from 64 in a memory: this fits the shape, but D3D11's code cannot be read, and on D3D12 it is not so (above).
- Zeroing is not what grows. Linear in N, it would double each step, and the steps shrink; 1024 to 2048 adds 2.4 ns an instruction, about 20 us a pass for 1,024 more `mov`s, so all of the zeroing is at most about 5 ns an instruction at 2048. No side without the zeroing was run.
- `rv_newisa`'s "another read of `xr`, 0 to 2 ns" is not contradicted: `xr` is 65 words, and those probes added a read of an array already there. But its "64 or 256 would be free to read" does not hold for an array read and written like this. The probes stored to `xr[64]`, which a driver could drop; this sink cannot be dropped.

Not run:
- A second load: Tiberian Dawn's snapshot was not run, so the table rests on gzip alone.
- An array of one read and no write, or `xr` itself made larger: this prices a new array read and written, not `xr` at 128.
- Driver register count: not visible from DXBC.
- `mctest-3` on WARP passed on `rvc_lean_sc` without a define (83 s, stamp `f735a07559ff2901`). With a define the tick ran on the card with no sum checked, so that the guest's behaviour is unchanged is by construction, not checked. Instructions a pass are level (8,076 to 8,145; the lowest are `uint4` at 256).
