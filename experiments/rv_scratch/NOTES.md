# A large scratch register file in the tick, priced on the card (1 November 2026)

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
- Up to 32 words the driver keeps the array in registers and reaches them by a dynamic jump or a walk, from 64 in a memory: this fits the shape, but the driver's code was not read (no SASS, no Nsight).
- Zeroing is not what grows. Linear in N, it would double each step, and the steps shrink; 1024 to 2048 adds 2.4 ns an instruction, about 20 us a pass for 1,024 more `mov`s, so all of the zeroing is at most about 5 ns an instruction at 2048. No side without the zeroing was run.
- `rv_newisa`'s "another read of `xr`, 0 to 2 ns" is not contradicted: `xr` is 65 words, and those probes added a read of an array already there. But its "64 or 256 would be free to read" does not hold for an array read and written like this. The probes stored to `xr[64]`, which a driver could drop; this sink cannot be dropped.

Not run:
- A second load: Tiberian Dawn's snapshot was not run, so the table rests on gzip alone.
- An array of one read and no write, or `xr` itself made larger: this prices a new array read and written, not `xr` at 128.
- Driver register count: not visible from DXBC.
- `mctest-3` on WARP passed on `rvc_lean_sc` without a define (83 s, stamp `f735a07559ff2901`). With a define the tick ran on the card with no sum checked, so that the guest's behaviour is unchanged is by construction, not checked. Instructions a pass are level (8,076 to 8,145; the lowest are `uint4` at 256).
