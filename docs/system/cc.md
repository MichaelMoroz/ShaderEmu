# A C compiler in the machine

The image has a C compiler that runs in the guest and makes programs for it: TinyCC, from
jrrk2's `riscv32` branch (32-bit RISC-V). `linux/tcc/build.sh` builds it, after
Nano-X's build.

    / # cc hello.c -o hello                 # the C library, Nano-X and OpenGL ES are linked in
    / # example cube                        # compiles /usr/share/example-cube.c and runs it
    / # example home
    / # nxide                               # write, build, run and debug it in a window (docs/apps/ide.md)

- `cc` is a script round `tcc -static`: programs here are static, as everything else in the
  image is. Its headers and libraries are files of the ROM, in `/usr/include` and `/usr/lib` (241 of
  them, 4.9 MB). They were one archive unpacked over the root at the first build, which was
  half of that build: 110 million instructions against 55 for the shader example in the
  world, where every page copied is a round.
- Without an MMU (`docs/system/nommu.md`) `cc` is `linux/nommu/cc`: it links the program as a library
  (`tcc -shared`, every address relocatable) and `elf2flt` makes that a flat program.
- What a program gets: musl's headers and `libc.a`, `libnano-X.a` with `nano-X.h`,
  `libgles.a` with `GLES/gl.h` and `GLES/segl.h` (`programs/linux/gles.c` in its float build:
  OpenGL ES 1 on the machine's GPU, `docs/machine/gpu.md`), and `libmcw.a` with `mcw.h` and `mcwx.h`: the worker
  cores (`docs/machine/multicore.md`, "Using the workers from a program", and "The second layer"), for a program that shares
  its work out as `example shader` does; and `libhlsl.a` with `hlsl.h`, which compiles an HLSL
  fragment shader for the lanes while the program runs (`docs/system/hlsl.md`, `example fractal`).
- `cc -g` writes stabs, which the IDE's debugger reads (`docs/apps/ide.md`).
- The C library's `sinf`, `cosf` and their like work in doubles, which are library calls
  here: a program that calls them for every pixel is seconds a picture. Write the few floats
  yourself (`wave` in the shader example).
- **Floats are instructions** (`docs/machine/fpu.md`): `cc` passes `-mfpu`, and `linux/tcc/fpu_hook.py`
  limits that option to single precision, which is all the machine has. A `double` is still a
  library call each, and a constant written without an `f` is a double.
- TinyCC links its own runtime (`libtcc1.a`) last, after the C library, so compiler-rt's
  builtins (64-bit division, soft float, which the C library needs too) are put into that
  archive.
- The examples are `linux/tcc/examples/*.c`, in the image as `/usr/share/example-NAME.c`, in
  the Start menu's Programming and in the IDE's File menu (the menu's and the desktop's
  entries open the source in the IDE; `example NAME` builds and runs one). `hello` prints; `cube` is the
  smallest OpenGL program (it hands a frame over with `seglSwapEarly`, `docs/machine/gpu.md`: nothing
it would write is read by the GPU, and its next frame is 33 ms away); `home` is a small place to walk round, a list of boxes drawn
  again only when the viewer moves; `shader` is a picture traced a pixel at a time on the machine's
  lanes (`mcw_shade`, `docs/machine/lanes.md`): the field's 3,906 where it has them, 63 to 80 pictures
  of 240 x 180 a second, or the workers'. A pixel's shader must stay under some 2,000 of TinyCC's
  instructions (four pixels are a lane's in one pass). The OpenGL ones are floats throughout: `GL_FLOAT` arrays, `glRotatef`.
- Compiling takes about 7 seconds for either example (4 to 6 s of the processor: most of it
  is reading the libraries), the first time as any other.
- TinyCC does not optimise. A program that needs speed is still better built on the host
  with the cross compiler (`linux/userland/toolchain.sh`).

Not checked: programs that use `double` heavily, `long double`, and anything the branch's own
test suite skips (21 of 151 tests).

## Fewer instructions to compile (12 October 2026)

`cc` on hello went from 11.0 million instructions to 8.1, the cube from 28.2 to 22.5, the
shader from 34.2 to 27.9, `tcc -c` of `home.c` (the IDE's check) from 18.4 to 14.4; the
outputs are the same bytes (md5 of `cc`, `cc -g` and `tcc -c` for the four examples).

- The libraries are mapped (`linux/tcc/mapped.c`, under tcc's open/read/lseek/close):
  linking hello was 2,144 system calls and is 399.
- `speed_hook.py`: short float and double literals are read by `literal.h` (whole-number
  arithmetic for floats, one double operation for doubles; against the C library on 30
  million numbers, none wrong) instead of musl's `strtof` in long doubles, which was a
  quarter of compiling `home.c`; and an archive's symbols are hashed once, not at every pass.
- tcc is built as one source (`ONE_SOURCE=yes`). `-O3` was 3.5% for 80 KB more of ROM: not taken.

## Fewer passes ended early (12 October 2026)

A pass of the machine keeps about 6 KB of new stores (`docs/machine/boot.md`, "Passes that end
early"), and TinyCC stored in bursts: `cc` on hello ended 51 passes of its own early on a
full write cache, and ends 28 (75 to 44 with the kernel's; 92.0% of the passes' instructions
to 93.1%). `linux/tcc/pass_hook.py`, after `speed_hook.py`; the outputs are the same bytes
and every compile is fewer instructions (hello 9.37 million to 9.12 with the shell's part,
the cube 25.3 to 24.6, `tcc -c` of `home.c` 14.84 to 14.76).

- A section's data from 4 KB up is pages of its own (`mmap`, 64 KB at first, four times as
  many when it grows, by `mremap`): it was copied whole at every doubling and its new half
  cleared, 200 KB of stores for hello.
- A file's buffer is not cleared before it is read into, and is 4 KB (8 KB read at once was
  more than a pass keeps); the table of identifiers, 64 KB of zeros as the program starts,
  is cleared only before a second file.
- An archive's index is looked at where the archive is mapped (`tcc_mapped` in `mapped.c`):
  30 KB of `libc.a` was copied to be read.
- The sorted symbols become the table (they were copied back over it), and with no
  relocations left to number again the table is not sorted in memory at all: `put_syms`
  writes its runs of local and other symbols in the order.
- The zeros between a file's parts are a write, not a call a byte (8,000 calls).

What still ends passes there of its own: a member's section or symbols over 4 KB copied out
of an archive (7), six in the writing of the file (`tcc_write_elf_file` with what is inlined
in it: not looked into further), every symbol's address stored when the program is laid
out (5). The single pages of the ROM (107 then, the mapped libraries) are the next part.

## Fewer pages of the ROM (12 October 2026)

A program takes a member here and a member there of an archive, and a page of a mapped
file that is touched is a fault, a page read from the ROM and a pass ended. `cc` on hello
read 57 pages of `libc.a` so (they lie over all its 754), 34 of `libtcc1.a`, and the first
page of the three others: 94 of its 107 copies from the ROM. A window in the kernel reads
mostly pages nobody wants there. The archives are laid out for it instead:

- `linux/tcc/pack.py` (run by the build, after the libraries are staged) links every example
  with the compiler built for the host, each archive's members named by their number, and
  reads from `-vv` which are taken. It writes each archive again: the index, then what the
  smallest example takes (hello: what any program takes), then what the others take
  besides, then the rest as it was. The index keeps its order and only its places change,
  so the linker takes the same members in the same order: the outputs are the same bytes.
  It writes no dates or owners into them, so the same sources are the same archives
  (they had the time of the build in them, and `linux/prebuilt` grew by it).
- It lists for the compiler the pages those parts end at (`packed.h`, compiled into
  `mapped.c`, which knows an archive by its name and length): windows of 32 pages at most,
  what one copy from the ROM reads. `libc.a`: 32, 64 and 74 (the index is 9 pages of the
  first, hello's 37 members the other 23); `libtcc1.a`: 21 and 28.
- `mapped.c` asks for a window (`madvise`, `MADV_WILLNEED`) the first time anything in it is
  read; the rest of the archive stays as it was, a page a fault.
- A page costs 2,380 instructions read by its fault, and 775 in a window plus 950 when it is
  first touched: a window pays in instructions when three fifths of it are used.

| | copies from the ROM | passes' instructions used | instructions, with the shell's part |
|---|---|---|---|
| `cc` on hello, and its run | 107 to 18 | 93.1% to 97.0% (97.2% in another run) | 9.12 million to 9.02 |
| the cube | 230 to 34 | 94.0% to 97.4% | 24.62 to 24.44 |
| home | 263 to 38 | | 30.43 to 30.11 |
| the shader | 246 to 44 | 97.5% now | 30.17 to 29.90 |

A program that is none of the examples (files, `qsort`, strings) finds most of what it takes
in the first two windows of `libc.a` and reads eleven pages more by themselves: 32 copies,
97.8%. To see a compile's copies by file, log the parallel copies of `rvc_cpu` (source,
length) and look the sources up in the ROM's directory; `cc -vv` prints the members taken.

What is left of hello's 18: the compiler's own file (8, the kernel's windows as it is
paged in), the last page of a file read by itself when it is 1 KB or more of a page
(`stdio.h`, `alltypes.h`, `tcc`, and all of `crt1.o`: the kernel's, `romfs_readahead`), and
the first page of each of the three libraries hello takes nothing of.
