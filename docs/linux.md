# The world on Linux

VRChat on Linux runs under Proton, where Direct3D is a layer over Vulkan: DXVK for
Direct3D 11, vkd3d-proton for Direct3D 12. Either way the world's shaders arrive as the
DXBC bytecode our compiler made (`fxc2.md`), and the layer turns each into SPIR-V before the
graphics card's driver sees it. Both layers now do that with one library, dxbc-spirv.

## What crashed (October 2026)

The tick's bytecode had six instructions the library gets wrong: `imul` and `umul` with a
high result and no low one,

    imul r436.x, null, r440.xxxx, r443.xxxx
    umul r436.x, null, r440.xxxx, r443.xxxx

which is `mulhi()`, the one-instruction MULH that `src/emu.h` used whenever fxc2 compiled
it. Microsoft's compiler has no way to be asked for it, so no game's shaders have it, and
the library's path for it was never walked: its scalarize pass takes the two-part result
for a vector and splits the instruction (`ir/passes/ir_pass_scalarize.cpp`,
`handleGenericOp` for `SMulExtended`).

- Built with assertions, the library stops there:
  `extractOperandComponents: Assertion 'first + count <= operandType.getVectorSize()' failed`.
- Built as the layers ship it, it goes on and writes SPIR-V that is not valid,

      %5874 = OpSMulExtended %_struct_5873 %2093 %2088
      %5875 = OpCompositeExtract %int %5874 0
      %5876 = OpCompositeExtract %int %5874 1
      %5877 = OpCompositeConstruct %int %5875 %5876

  (`spirv-val`: "Expected Result Type to be a composite type"), six times, once for each of
  those instructions. A driver handed that may do anything, which fits a crash on every
  make of card.

Not seen here, only reasoned: the crash itself (this was found in WSL, which has no Vulkan
driver to hand the module to), and which of the two layers VRChat is on. The library also
makes a signed multiply of `umul` as far as its converter reads (`dxbc_converter.cpp`,
`handleIntMultiply`), so MULHU might be wrong there even once the module is valid; that was
not tested either.

## What was done

`NO_MULHI` keeps the high word from 16-bit partial products, as Microsoft's compiler gets
it, under fxc2 too. `MachineTick.shader` defines it. With it all eleven stages the machine
has (tick, commit, GPU, sound) go through the library with assertions on and pass
`spirv-val`; without it the tick is the one that does not. It costs nothing that can be
measured (Doom's demo: 3.60M instructions a second with it, 3.67M without, the same state
sum; MULH is a rare instruction).

The harness still uses the one instruction (Direct3D on Windows has no such trouble):
`--define NO_MULHI` builds what the world has.

## Checking a shader before it ships

`tools/linux_spirv` (WSL; nothing is installed system-wide, everything goes to `~/vrc`):

    bash tools/linux_spirv/get_dxbc_spirv.sh      # the library and ninja, from their sources
    bash tools/linux_spirv/build_tools.sh         # its dxbc_compiler and dxbc_disasm
    bash tools/linux_spirv/validate.sh            # spirv-val, from Ubuntu's package, unpacked
    bash tools/linux_spirv/check_cache.sh DIR     # every .cso in a shader cache folder

Compile the machine into an empty cache folder first, with the world's switches:

    bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --image linux-net --no-stdin --no-mrt --define L1_TABLE_BITS=6 --define NO_MULHI --cache build\cache_vrc --until "/ # " --seconds 60

and `check_cache.sh` must end in "0 do not". Anything new that fxc2 can write and
Microsoft's compiler cannot is a thing to run through this before a world is uploaded.
Upstream vkd3d (Wine's, which Proton does not use for games) refuses the same instruction
outright: "Extended multiplication is not implemented".
