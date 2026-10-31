# A C compiler in the machine

The image has a C compiler that runs in the guest and makes programs for it: TinyCC, from
jrrk2's `riscv32` branch (32-bit RISC-V). `linux/tcc/build.sh` builds it, after
Nano-X's build.

    / # cc hello.c -o hello                 # the C library, Nano-X and OpenGL ES are linked in
    / # example cube                        # compiles /usr/share/example-cube.c and runs it
    / # example home
    / # nxide                               # write, build, run and debug it in a window (docs/apps/ide.md)

- `cc` is `linux/nommu/cc`, a script round `tcc`: it links the program as a library (`tcc -shared`,
  every address relocatable) and `elf2flt` makes that a flat program (`docs/system/nommu.md`). Its
  headers and libraries are files of the ROM, in `/usr/include` and `/usr/lib`.
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
  lanes (`mcw_shade`, `docs/machine/lanes.md`): the field's where it has them, or the workers'. A
  pixel's shader must stay under some 2,000 of TinyCC's instructions (four pixels are a lane's in
  one pass). The OpenGL ones are floats throughout: `GL_FLOAT` arrays, `glRotatef`.
- TinyCC does not optimise. A program that needs speed is still better built on the host
  with the cross compiler (`linux/userland/toolchain.sh`).

Not checked: programs that use `double` heavily, `long double`, and anything the branch's own
test suite skips. `python tools/test_all.py cc-bytes` compiles the four examples with `cc`, `cc -g`
and `tcc -c` and compares the outputs' md5 sums.

## What a compile costs

On `rvc_cpu`, 31 October 2026, with the shell's part:

| | million instructions | seconds |
|---|---|---|
| `cc` on hello, and its run | 8.7 | 1.8 |
| `cc` on the cube | 22.5 | 4.5 |
| `cc` on the shader | 30.4 | |
| `tcc -c` of `home.c` (the IDE's check) | 13.0 | |

The seconds are calibrated, 31 October 2026.

## Fewer instructions to compile

- The libraries are mapped (`linux/tcc/mapped.c`, under tcc's open/read/lseek/close), not read
  by system calls.
- `speed_hook.py`: short float and double literals are read by `literal.h` (whole-number
  arithmetic for floats, one double operation for doubles) instead of musl's `strtof` in long
  doubles; and an archive's symbols are hashed once, not at every pass.
- tcc is built as one source (`ONE_SOURCE=yes`).

## Fewer passes ended early

A pass of the machine keeps about 6 KB of new stores (`docs/machine/boot.md`, "Passes that end
early"), and TinyCC stores in bursts. `linux/tcc/pass_hook.py`, after `speed_hook.py`, leaves the
outputs the same bytes:

- A section's data from 4 KB up is pages of its own (`mmap`, 64 KB at first, four times as
  many when it grows, by `mremap`), not copied whole at every doubling.
- A file's buffer is not cleared before it is read into, and is 4 KB; the table of identifiers,
  64 KB of zeros as the program starts, is cleared only before a second file.
- An archive's index is looked at where the archive is mapped (`tcc_mapped` in `mapped.c`).
- The sorted symbols become the table, and with no relocations left to number again the table
  is not sorted in memory at all: `put_syms` writes its runs of local and other symbols in the
  order.
- The zeros between a file's parts are a write, not a call a byte.

## Fewer pages of the ROM

A program takes a member here and a member there of an archive, and a page of a mapped file
that is touched is a fault, a page read from the ROM and a pass ended. A window in the kernel
alone would read mostly pages nobody wants there, so the archives are laid out for it:

- `linux/tcc/pack.py` (run by the build, after the libraries are staged) links every example
  with the compiler built for the host, each archive's members named by their number, and
  reads from `-vv` which are taken. It writes each archive again: the index, then what the
  smallest example takes (hello: what any program takes), then what the others take
  besides, then the rest as it was. The index keeps its order and only its places change,
  so the linker takes the same members in the same order: the outputs are the same bytes.
  It writes no dates or owners into them, so the same sources are the same archives.
- It lists for the compiler the pages those parts end at (`packed.h`, compiled into
  `mapped.c`, which knows an archive by its name and length): windows of 32 pages at most,
  what one copy from the ROM reads.
- `mapped.c` asks for a window (`madvise`, `MADV_WILLNEED`) the first time anything in it is
  read; the rest of the archive stays as it was, a page a fault.

To see a compile's copies by file, log the parallel copies of `rvc_cpu` (source, length) and
look the sources up in the ROM's directory; `cc -vv` prints the members taken.
