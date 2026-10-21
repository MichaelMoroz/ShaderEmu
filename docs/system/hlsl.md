# HLSL in the machine

A fragment shader written in HLSL is compiled in the guest, when a program runs, to RISC-V code
that runs on the lanes (`docs/machine/lanes.md`). The compiler is `programs/hlsl` (`hlsl.h`),
`libhlsl.a` in the image, and `cc` links it. Neither the host nor fxc is involved.

    hlsl_program *p = hlsl_compile(source, "main", error, sizeof error);
    hlsl_set(p, "eye", eye, 3);                     // a uniform
    hlsl_draw_begin(p, pixels, 160, 120);           // the field's lanes, the workers', or this core
    while (!hlsl_draw_ready(p)) mcw_rest();

The entry is `float4 main(float4 pos : SV_Position) : SV_Target`; `pos.xy` is the pixel's
centre, from the top left. The colour is stored as RGBA bytes, clamped.

## The language

Scalars, vectors and matrices of `float`, `int`, `uint` and `bool`; structs; `static const`
tables; functions with `in`, `out` and `inout`; `if`, `for`, `while`, `do`, `break`,
`continue`, `return`; swizzles on both sides of `=`; `[unroll]` and `[loop]`; object-like
`#define`. A global that is not `static` or `const` (or a `cbuffer`'s member) is a uniform.
The intrinsics are those of arithmetic and geometry (`dot`, `cross`, `normalize`, `length`,
`reflect`, `mul`, `lerp`, `smoothstep`, `clamp`, `saturate`, `frac`, `fmod`, ...), and `sin`,
`cos`, `tan`, `atan`, `atan2`, `exp`, `exp2`, `log`, `log2`, `pow`, `sqrt` and `rsqrt` made of
the machine's instructions. There are no textures, no derivatives and no doubles.

## The code

- Every call is inlined, every vector is split into scalars, constants are folded, and a loop
  of a known count is unrolled when the copies are small enough (count at most 32, and at most
  640 operations in all) or when it says `[unroll]`.
- Every other loop may stop on its back edge: it reads what the pass has left (CSR `0x0ba`)
  and stops if that is less than the longest way to the next place it may stop, which the
  compiler measures on its own code (`hlsl_stats`' `longest_step`). Stopping stores the live
  values in the index's texels, and the next round loads them and goes on (`mcw_step_fn`,
  "Steps" in `docs/machine/lanes.md`). A shader can therefore march for as long as it likes.
- An index is four pixels side by side, so a lane's texels hold four colours; an index keeps
  `1 + ceil(state / 4)` texels, at most 6. A picture's width is a multiple of four.
- Registers are given by linear scan; a constant is loaded once where it is first needed, or
  reloaded instead of spilled.
- `hlsl_draw` runs the steps on the field (`mcw_field_steps`), then gathers the colours into
  the picture with a second field job. Without a field it uses the workers' lanes
  (`mcw_lanes_steps`), and without workers it makes plain calls on the first core.

## The example

`linux/tcc/examples/fractal.c` with `fractal.hlsl` (`/usr/share/example-fractal.*`, Start
menu, Programming): a Sierpinski pyramid, the attractor of an iterated function system of
eight folds, ray marched. The program compiles the `.hlsl` when it starts and only sets the
camera; drag, the arrows or w a s d go round it, the wheel or + and - come nearer, space stops
it.

- The compile in the machine takes 7.6 million instructions: 1.3 s on the card.
- 10,828 bytes of code, 2 places a lane may stop, 3 texels an index, the longest step 2,662
  instructions, about 5,800 instructions a pixel.
- 160 x 120 on the card's field: 86 ms a picture in 14 rounds (`hlsl_bench`). The example on
  the desktop shows 9 to 11.8 pictures a second, and 6.6 to 8.3 when close, where rays take more
  steps. `cc` builds it in the machine in 7.7 s. On `rvc_cpu` a picture takes 34.5 s on the
  first core alone and 4.3 s on 8 workers.

## In the IDE

A `.hlsl` file in the IDE (`docs/apps/ide.md`) is coloured, checked while typing by `hlslc`
with its mistakes marked at their lines, and run by Run in `hlslview` (the example, built at
image time). File, Examples has the example's C and its shader. `test_all.py nxide-hlsl`.

## Tests

- `programs/hlsl/test/hlsl_test.c` on the host (`gcc -O2 -I.. hlsl_test.c ../hlsl.c
  ../hlsl_parse.c ../hlsl_gen.c ../hlsl_back.c -lm`): an RV32IMF emulator runs the compiled
  code against expected values, and in steps under random budgets against the whole calls.
  With a file, size and uniforms it draws a picture and prints its sum.
- `hlsl_bench FILE W H PICTURES [name=v,...]` in the machine (`HLSL_WORKERS=n` opens workers
  where there is no field) prints the compile's time and the pictures' times and sum. On
  `rvc_cpu` the sum is the host's; on the card float results differ (`test_all.py hlsl`).
