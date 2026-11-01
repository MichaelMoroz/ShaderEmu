# The striped write cache: a trace's answer (1 November 2026)

The question: can core 0 hold more stored texels at the same array size, by several blocks of pixels in lockstep that each keep the data of a share of the addresses? A load of a texel another block holds ends the pass in all.

**Answer: no, with any policy found that a tick could run. Nothing was built or run on the card.** Everything below is modelled from `gvm_cpu` traces of core 0 (`GVM_STLOG` with loads; bit 23 of the record, a silent store, added on GPU-VM's `isa-cache`).

| file | what |
|---|---|
| `trace.py` | the traces, in WSL's `~/stripes_trace` |
| `sim.c`, `stats.sh`, `grid.sh`, `more.sh`, `pred.sh` | the model and its runs: `stats.txt`, `grid.txt`, `more.txt`, `pred.txt` |
| `oracle.c` | the ceiling: what is kept by all chosen knowing the pass's future |
| `storedonly.c`, `storedonly.sh` | stored only against read back, windows of about 8,000 ops: `storedonly.txt` |

Budget 8,192 bundles. The stream is cut again by each model: only a logged sync, wait or halt is a real end. The model's full share is below the card's at 168 entries (gzip 11 against 21%, png 18 against 28%, Tiberian Dawn 54 against 59%), so read its absolute shares as low.

## Why it fails

Without a part kept by all, a pass lasts 29 to 438 bundles: the first read-back of a stored texel comes at bundle 79 to 585 (png 2,180). So some texels must be in every block, and those take the array that stripes were to save. What decides which:

| bundles a pass, silent stores on | gzip | png | mp3 serial | jpeg | boot | Tiberian Dawn |
|---|---|---|---|---|---|---|
| unstriped, 168 entries | 7,969 | 7,282 | 4,360 | 3,400 | 4,252 | 6,205 |
| unstriped, 224 (the same array) | 8,067 | 7,681 | 5,570 | 4,153 | 5,183 | 6,666 |
| 8 stripes of 168, 128 kept by all, LRU | 7,999 | 7,297 | 3,974 | 4,772 | 4,060 | 6,294 |
| 8 stripes of 224, the first 128 of a pass kept by all | 7,988 | 7,415 | 4,172 | 4,714 | 4,775 | 6,797 |
| the same, marked by read-backs, table of 2,048, 16 texels a bit | 7,302 | 4,322 | 4,601 | 1,678 | 2,525 | 5,938 |
| 128 kept by knowing the future, stripes unlimited (not runnable) | 8,156 | 7,939 | 7,139 | 6,298 | 6,955 | 7,267 |
| unstriped, 720 | 8,154 | 7,948 | 7,173 | 6,098 | 6,923 | 7,187 |

The LRU row has 296 entries a block and the first-128 row 352, more than the array allows. Against 224 unstriped they gain only on jpeg (177 windows) and, first-128, 2% on Tiberian Dawn; they lose on mp3 and boot. The gain the ceiling shows is in knowing which stores will be read: 36 to 83% of a window's texels are stored only, and LRU fills with them. Learning that by address did worse than LRU.

A filter in place of tags only adds false ends: Tiberian Dawn at 16,384 bits 4,228 bundles a pass against 6,294 exact, at 4,096 bits 1,647.

## Agreement

Worked out, not run. Tags for all in every block agree exactly, and at about 5 KB a lane fit 4 x 156 or 8 x 104 entries. A filter alone cannot: only the holder knows its table is full. A silent store agrees, since a texel not yet marked reads the same RAM in every block and a marked one changes no mark.

## Not tried

- A store's kind told by the compiler or the ISA (a bit in the op: "will be read in this pass"): the one thing the ceiling leaves open. It is outside this question's tick-only hash of addresses, though the ISA is allowed. Whether a compiler can know it is unknown.
- Silent stores alone on Tiberian Dawn on the card: modelled at 168 entries 54 to 23% full, 4,515 to 6,205 bundles a pass.
