# A C compiler in the machine

The image has a C compiler that runs in the guest and makes programs for it: TinyCC, from
jrrk2's `riscv32` branch (32-bit RISC-V). `linux/tcc/build.sh` builds it, after
Nano-X's build.

    / # cc hello.c -o hello                 # the C library, Nano-X and OpenGL ES are linked in
    / # example cube                        # compiles /usr/share/example-cube.c and runs it
    / # example home

- `cc` is a script round `tcc -static`: programs here are static, as everything else in the
  image is. The first run unpacks headers and libraries from `/usr/share/cc-sysroot.tar`
  (4.6 MB) over the root, which is an overlay in RAM: the ROM's builder cannot make folders.
- What a program gets: musl's headers and `libc.a`, `libnano-X.a` with `nano-X.h`, and
  `libgles.a` with `GLES/gl.h` and `GLES/segl.h` (`programs/linux/gles.c` in its float build:
  OpenGL ES 1 on the machine's GPU, `docs/gpu.md`).
- **Floats are instructions** (`docs/fpu.md`): `cc` passes `-mfpu`, and `linux/tcc/fpu_hook.py`
  limits that option to single precision, which is all the machine has. A `double` is still a
  library call each, and a constant written without an `f` is a double.
- TinyCC links its own runtime (`libtcc1.a`) last, after the C library, so compiler-rt's
  builtins (64-bit division, soft float, which the C library needs too) are put into that
  archive.
- The examples are `linux/tcc/examples/*.c`, in the image as `/usr/share/example-NAME.c` and
  in the Start menu. `cube` is the smallest OpenGL program; `home` is a small place to walk
  round, a list of boxes drawn again only when the viewer moves. Both are floats throughout:
  `GL_FLOAT` arrays, `glRotatef`, `sinf`.
- Compiling takes about 7 seconds for either example (4 to 6 s of the processor: most of it
  is reading the libraries), and 10 s more the first time, for the unpacking.
- TinyCC does not optimise. A program that needs speed is still better built on the host
  with the cross compiler (`linux/userland/toolchain.sh`).

Not checked: programs that use `double` heavily, `long double`, and anything the branch's own
test suite skips (21 of 151 tests).
