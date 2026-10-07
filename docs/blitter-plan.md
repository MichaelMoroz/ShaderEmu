# Plan: a blitter in the commit pass

Not built. This is the plan for widening the machine's parallel memory operation into a
small blitter that programs can use, and the measurements that say what to build first.

## What exists

The commit pass can already do one memory operation a frame instead of applying the write
cache: a copy or a fill (`CSR_MEMOP_*`, CSRs `0x0b0`-`0x0b3`: operation, source or fill word,
destination, length; writing 1 or 2 to `0x0b0` runs it and ends the CPU's frame). Its limits:

- kernel mode only, and addresses in the kernel's linear map;
- source, destination and length multiples of 16 bytes (a texel), at least 1 KB;
- one operation a frame.

The kernel's `memcpy`, `memset` and the ROM driver use it (`linux/kernel/*_hook.py`).

## Why widen it

Measured while speeding Doom up (`docs/doom.md`):

- `copy_to_user` was 4 to 8% of all instructions in a profile and ended hundreds of frames:
  file reads are not texel-aligned, so they go a word at a time through the write cache.
- Doom's own large copies and fills (a 64 KB page, textures, clears) did the same from user
  mode, where the operation cannot be reached at all.
- Copying columns into a row-major picture was far worse than its size: stores a row apart
  share a few sets of the write cache, which filled every few dozen bytes. Textures are kept
  on their side now to avoid it. A copy with a stride would not have needed that.

Each of these was worked around by hand in one program. The operation would fix them for
every program.

## What the commit pass can compute

One fragment runs for each destination word, so the pass gathers:

    dst[i] = f(a[g(i)], b[h(i)], constants)

| Fits | Does not fit in one pass |
|---|---|
| a copy with any stride or row length: rectangles, transposes | a result made from many words: `strlen`, `memchr`, `memcmp`, sums |
| an overlapping move (the pass reads the old state: `memmove` for nothing) | scatter: histograms, sorting |
| a fill, a pattern | an output that depends on an earlier output |
| logic and saturating add a byte at a time; select by a mask or a key | |
| a table lookup (a palette), bytes repacked (three bytes a pixel to words) | |

That is a blitter in the old sense: two sources, a mask, a few ways to combine them, and a
row length for each operand. It is not a program a texel: an interpreter would run for every
texel of every band the commit draws (the whole state texture each round in the VRChat
world), and a `switch` costs about 4 ns a case in these shaders. A range test and a handful
of fixed operations stay cheap.

The GPU device already gathers into any rectangle of RAM, but it writes colours: 8 bits a
channel into `0x00RRGGBB` words. It cannot move 32-bit data exactly or address bytes. The
blitter is its counterpart for data.

## Where it will not help much

- Drawing on the desktop. Windows are render targets of the GPU already and a string is one
  GPU command. A drawing request costs 1,000 to 2,000 instructions whatever it draws, and the
  first Nano-X profile was 57% system calls: pixels are not where that time goes.
- Small operations. Each one ends the CPU's frame, so below a few hundred words the write
  cache is the cheaper way.

## Steps

Each step changes the commit shader (an FXC compile, and a recompile of `Machine.shader` in
Unity, not of the four-minute tick), and each must leave the state hashes of a run that uses
none of it unchanged.

### 0. Measure first

Count frames that end in `copy_to_user`, `memcpy`, `memset` and their user-mode namesakes
(`--frame-log`, with symbols) for: the desktop starting, a 5 MB file read, a program starting,
Doom loading a level and playing. That says how much step 1 is worth and whether 2 to 4 are
worth building at all. Expect step 1 to be most of the value.

### 1. Any alignment, and from user mode

- Source and destination at any byte: the fragment for a destination word reads the one or
  two source words that hold its four bytes and shifts them into place. The first and last
  words of the range keep the bytes outside it.
- A lower threshold, found by measuring (the break-even against the write cache).
- User mode: the operation takes physical ranges, and a user buffer is not contiguous beyond
  a page. Two ways, in order of effort:
  1. the kernel's `copy_to_user` and `copy_from_user` use it a page at a time, for pages that
     are present and writable (anything else takes the path there is now);
  2. a call through `/dev/gpu` for memory mapped from it, which is contiguous: one operation
     for the whole range. The C library's `memcpy`, `memmove` and `memset` use it for ranges
     inside that mapping above the threshold.
- Check: a test program copies and fills at every alignment and length from 1 to 4,099 bytes,
  overlapping both ways, and compares with a byte loop; then the file-read benchmark and
  Doom's level load, in instructions and in machine frames.

### 2. A row length for each operand

- Operands become (address, row length in bytes, width, height): a rectangle out of a wider
  picture, a scroll inside one, and with rows and columns exchanged a transpose.
- Check: scroll a window's buffer by a row and by a column and compare with the software
  move; build one of Doom's wall textures upright with it and compare with the copy by hand.

### 3. A second source and a few ways to combine

- Operations: copy; select by key (a byte or a word); and, or, xor; add and subtract a byte
  at a time, saturating; look up through a table of 256 words (a palette); three bytes a
  pixel to words and back.
- Check: each operation against a C loop over random data, on both backends.

### 4. Several operations a frame

- A queue of a few descriptors (four to eight) in the state texels, so a program's burst of
  small operations shares one end of frame. The commit tests a destination word against each
  queued range, later ones winning.
- Check: queued operations that overlap give what running them one after another gives.

## Open questions

- Whether an operation should see the same frame's write-cache stores. Today the operation
  replaces the cache's commit for that frame; a queue makes the order matter.
- How the commit's band list (the 4 MiB bands it draws on D3D12) takes ranges that the CPU
  never wrote to.
- What the threshold is in the VRChat world, where a round costs more than a harness frame.
