# The GPU's VRAM: where it stands and what is left (5 November 2026)

The branch `vram-gpu` is work in progress on the GPU device (`shader_vm/src/gpu.h`, `docs/machine/gpu.md`, "VRAM"). It
was asked for as: the GPU shader at least 3 times cheaper with every feature kept, textures in a VRAM texture sampled by
the card's samplers with mip levels, geometry in a float VRAM, mipmapped textures by default in every game that can, a
shared math library, a trim of `shader_vm/src`, and a profile of every device in Unity. What follows is what is done,
how it was checked, and what is left, in the order it is worth doing.

## Done

- **Texture VRAM.** A 4096 x 4096 RGBA8 texture with hardware mip levels (`GPU_VRAM`). A texture address whose top two
  bits are clear is a slot: `x | y << 12 | L << 24 | flags` (`VRAM_PREMULTIPLIED` 0x08000000 for a keyed texture,
  `VRAM_CLAMPED` 0x10000000). A block is padded 2^L texels each way (wrapped or clamped), its corner and size are
  multiples of 2^L, and its LOD is clamped to L (at most 4), so trilinear is one `SampleLevel`. Keyed textures are
  premultiplied: key texels 0, divided by alpha, discarded under 0.5.
- **`CMD_UPLOAD` (op 6)** and the `GPUUpload` pass (`gpu_upload_fragment`), drawn every frame a list is drawn, level 0
  only: the host makes the levels (`GenerateMips`) only when a list sets `SUBMIT_UPLOADS` (8), which it does only when
  one of its uploads has L > 0; such a list waits for `GPU_UPLOADED` (bit 16 of `_GpuPasses`). Light maps (L 0) wait for
  nothing.
- **Harness** (`rvc_backend11.cpp`, `rvc_harness.cpp`): the VRAM, the pass, `GenerateMips`, `--save-state` writes
  `FILE.vram` and `--load-state` reads it, `RVC_PASS_PROF` labels "gpu draw" and "control+sound".
- **Guest library** (`programs/linux/gles.c`, `segl.h`, `programs/common/gpu.h`): `seglVram(lod)` (`SEGL_VRAM` in the
  environment; off unless a program asks), `seglTextureChanged()` after a texture's pixels are written,
  `seglVramPalette(words)` for indexed textures, a shelf allocator in 16-texel steps with a free list, `gpu_upload()`.
- **Quake**: VRAM with 4 levels by default (`QUAKE_VRAM=0` the way before). e1m1 on WARP: distant floors smooth,
  GPU memory for textures 2059 to 1543 KB.
- **Doom**: VRAM with 4 levels by default (`DOOM_VRAM=0` the way before), the palette's flash drawn as a tint over the view
  (`doom_tint`), and the stretched floors fixed (`flat_origin`: a flat's texture coordinates are taken from the piece's
  own origin; 16-bit 1/1024 coordinates overflowed beyond 2048 units). The plain palette is read from `PLAYPAL` when the
  GPU opens (the game's first `I_SetPalette` comes before). `test_all.py doom-1 doom-told doom-rest` pass on a private
  image; E1M1 captured on WARP is the picture of `DOOM_VRAM=0` but smoother far away. A change of gamma does not upload
  the pictures again.
- **`programs/gpubench`**: a bare program in the kernel's place drawing 1280 x 720 x 8 layers in nine modes
  (`gpubench0` to `8`, images by `python tools/make_lazy_image.py build/gpubench/mN --program gpubenchN`). On the RTX
  5090, GPU draw ms: no list 0.036, colour 0.055, words nearest 0.055, words smooth 0.104, indexed nearest 0.075, indexed
  smooth 0.14, RGB24 smooth 0.22, compact with a light map 0.174; from the VRAM, words smooth 0.053 and compact with a
  light map 0.062. A textured fragment now costs what a flat one does.
- **Unity** (not run yet: the editor's MCP bridge was not reachable): `GpuVram.renderTexture` (`ShaderEmuBuilder.Vram()`),
  GpuDrawBlit's pass 13 `GPUUpload`, `EmuMachine.VramFrame()` and the blit and `GenerateMips` in `GpuDraw`, `_GpuUploaded`
  in `Machine.shader` and `GpuDrawPass.cginc`, `_GpuVram` on the volume, holodeck and HUD materials (`GPU_VRAM` defined in
  their passes). `GenerateMips` is in the Udon wrapper.
- **`shader_vm/src/types.h`**: 29 lines no file used are gone (`glsl_mod`, `CORE0_ROWS`, `fast_budget`, the probe statics,
  `L1_KNOWN`, `L1_REMEMBER`, `L1_HAS`). It cannot go whole: the commit shader and Unity's Unpack pass include it. The
  shaders are stamped (`shader_vm/warp_ok`).

## Left, in order

1. **Unity.** Run "Sync shader sources", rebuild the world (`ShaderEmuBuilder`), compile every shader, play Doom and Quake
   in the Game view and compare with the harness. Check the holodeck and the volume display read `_GpuVram` (a seated
   Quake). Then the profile of every device in Unity (`docs/machine/multicore.md`, "In Unity"): the ticks, commit, GPU
   passes, upload pass, sound, room, by `totalInstructions` and the frame's GPU time.
2. **The vertex cost.** Done in the harness (5 November 2026, `docs/machine/gpu.md`, "Claims"): a claims pass finds every
   vertex's command and passes once a frame, and the upload pass and the 12 passes read one texel a vertex in place of the
   search. Checked by `GPU_CLAIMS_CHECK` (Quake, OpenTTD, gpubench 9, the blits). On the RTX 3090 it is 0.020 ms a frame
   of the GPU's draw in every gpubench mode (9: 0.142 to 0.122 ms, 2: 0.299 to 0.278, 8: 0.170 to 0.149): not the 3x
   hoped for, as what is left is 13 x 196,608 vertex invocations a frame that each load their claim and stop. The next
   cut is fewer invocations: a draw of only the list's vertices (its last command's end), which the host does not know
   without a readback a frame late, or the passes' draws limited by an indirect count (not in VRChat). Left: **Unity** has the search still
   (`GpuDrawPass.cginc` does not define `GPU_CLAIMS`): it wants a `GPUClaims` pass in `GpuDrawBlit.shader` blitted into a
   512 x 384 RGInt render texture before the upload pass (read as `int2`, `asuint`), `_GpuClaims` set globally, and
   `GPU_CLAIMS` in the draw and upload passes; the volume and holodeck passes too. A float VRAM of transformed vertices
   was the first plan; with the claims a vertex is transformed once a frame already (in its own pass), so it would save
   only the passes' few loads of a drawn vertex and cost 96 bytes a vertex written and read: not done.
3. **Other games into the VRAM** with `seglVram` and `seglTextureChanged` where they write a texture's pixels: Quake III
   (`gpu_shaderemu.h`, its own path; light maps from the ROM), Half-Life (`gl_shaderemu.c`), Jedi Outcast (`gl_jk2.c`),
   ClassiCube, OpenTTD's sprites if they gain. Quake's light maps into the VRAM at L 0, uploaded again when they change.
   Each game keeps a switch for the way before and its sums (`test_all.py` and its captures), as Doom's do.
4. **The hand path's uploads** in `gles.c` (`seglSwapHand`): an upload a worker's list asks for and one of core 0's can
   race; check with `RVC_RACES` on a program that uses both.
5. **Tools**: `tools/gpu_reference.py` does not know `CMD_UPLOAD` or slots; the D3D12 backend has no VRAM.
6. **The shared math library** for games and graphics (vector and matrix types, special functions), not started.
7. **`shader_vm/src` trim**: the `PASS_TICK` part of `types.h` that only Unity's Unpack pass and `mc.h` reach, and the
   other files of the folder; check by compiling every pass in the harness (a short `--warp` run), the stamp, and Unity.

## How to check

- The harness: `build.bat`, then `python tools/warp_stamp.py --count 2 --secs 8` after any change to `shader_vm`.
- A game: build it (`wsl -- bash linux/nommu/build.sh nanox/doom.sh`), make it flat into a copy of the root, make a
  private image (`linux/nommu/image.py NAME ROOT build/nommu/Image`, about 70 s), capture with
  `tools\gpu_queue.ps1 bin\rvc_harness.exe --cpu --warp --d3d11 --rvc shader_vm --define NO_PAGING --image build\images\NAME
  --no-desktop --no-viz --no-stdin --input "nano-X -p & sleep 1; doom -warp 1 1\n" --seconds 40 --gpu-capture FILE.bmp`,
  once with the game's VRAM switch at 0. `--save-state FILE` also writes `FILE.vram` (4096 x 4096 x 4 bytes, level 0):
  colours all 0 there is a palette not yet filled.
- Card timings: `RVC_PASS_PROF=1` with the `gpubench` images, `--image` the image's folder
  (`build\gpubench\mN\build\images\linux`: `linux-net` by name is refused on the card) and `--define NO_PAGING`.
  gpubench 7 and 8 (uploads) draw nothing under `--cpu`, the search of before too: not looked into.
