# The GPU reads the VRAM alone (plan, 5 November 2026)

The developer's goal: no RAM path in the GPU's drawing passes or the holodeck; RAM is read only to copy into the VRAM
(the upload pass) and by the front end that makes the vertices and the draw IDs. The GPU shader at least 2x simpler, with
fewer cases, no switches, as few defines as possible and half the code; faster than the RAM path; every texture sampled by
the card (trilinear or point), no bilinear of our own.

## The design

- **Draws**: every texture, light map, glyph and sprite record is a VRAM slot. A draw of an address that is not a slot is
  magenta. `gpu_fragment` is one `SampleLevel` (linear or point) per kind; masks, coverage and MSDF read the slot too.
- **Uploads**: `CMD_UPLOAD` is the one way into the VRAM. A program's library calls `gpu_list_uploads()`
  (`programs/common/gpu_uploads.h`) on its finished list before submitting it: each RAM or ROM texture a command names
  gets a block in the per-list scratch band (VRAM rows 3072 to 4095, shelves, a texel of padding, no levels), an upload is
  appended, and the command is rewritten to the slot. Lists are drawn one at a time and their uploads first, so every
  program's lists share the band. Kept commands and textures with persistent slots (gles.c's VRAM) are left as they are.
  - text: a table of the characters' glyph slots in the list's memory (`TEXT_SLOTS`), the front end reads it;
  - sprite records: an upload kind that makes the record's pixels through its remaps (`FRAGMENT_SPRITE` in the upload
    pass); a fill's and a checker's colour are the vertex's (the front end looks the palette up);
  - light maps: word 13 of a lit draw.
- **Gone from the device**: the device streams (six sums passes, the stream pass, the per-triangle items), `gpu_texel`,
  `gpu_msdf_pair`, the RAM branch of every fragment kind, `gpu_command_search` and `gpu_command`, the non-front-end vertex,
  `GPU_VRAM`, `GPU_STREAMS`, `GPU_VRAM_ONLY`, `GPU_NO_STREAMS`, `GPU_EDGE`, `GPU_DRAW_IDS`, `GPU_FRONT`, the checks
  `GPU_DRAW_IDS_CHECK` and `GPU_FRONT_CHECK`, `GPU_*_TEXEL`/`_WORD`/`_WORDS` (the integer targets are signed everywhere),
  the harness's `RVC_GPU_COMPARE`, `RVC_GPU_READS` (now a check that always runs), `RVC_STREAM_*`, `RVC_GPU_WHOLE_DEPTH`.
- **Holodeck**: the front end also writes each vertex's place in its program's camera space, its kind (world or flat) and
  its projection (planes, focal), and a frame pass the first world draw's camera and the up axis; the volume and the HUD
  layer draw the device's last list from those, reading no RAM (and so never a list being written).
- **Unity**: the same passes wired (GPUVertices with eight targets, GPUCopy, the frame pass); `GpuDraw.shader`'s camera
  mesh goes.

## Producers to change (the inventory of 5 November 2026)

| Producer | What it sends from RAM/ROM |
|---|---|
| `programs/linux/gles.c` (`stream_put` becomes `gpu_list_uploads`) | every GL port: words, indexed, mask, RGB24, tiles, YUV; light maps; textures over 254 rows |
| `linux/nanox/scr_shaderemu.c` | every list's surface as words (a window or 1024 x 768), blits, masks, pictures, text |
| `linux/apps/nxweb.c` | ROM pictures, glyphs |
| `linux/openttd/shaderemu_gpu.cpp` | sprite records, remaps, the last picture (`SPRITE_WORDS`) |
| `linux/quake3/gpu_shaderemu.c` | ROM pictures when the VRAM is full, the light-map page (word 13) |
| `linux/halflife/gl_shaderemu.c` (Jedi Outcast too) | words with holes, the light atlas |
| `linux/classicube/Graphics_ShaderEmu.c` | textures with no slot |
| `linux/tdawn`, `linux/ralert` `host.c` | pages, the atlas, masks, shades |
| `linux/micropolis/nxmicropolis.c` | the map's tiles, sprites, tools |
| `linux/aoe/port/window.c` | the page |
| `linux/video/nxvideo.c` | the frame (YUV, indexed, words) |
| `programs/common/gpu.h` (`gpu_image`, `gpu_draw`) | gears, blend, gpubench |

Each is rebuilt and captured (`rvc_harness --cpu --warp`, cropped to the picture) against the tree before it; the shader
change lands with the last of them. Then the card's draw time against the RAM path (`GPU_NO_STREAMS` of 5 November).

## OpenTTD's sprites

A per-list upload a record does not fit: a frame has up to 32,768 records (6 mesh vertices each already), against 4,096
commands and 196,608 vertices a list. The sprite cache uploads a sprite once into a persistent slot when it is cached (its
indices, a byte a pixel as R8 in a slot), the remap tables are 256 x 1 slots, and the draw reads the index and its remap from
the VRAM (two loads, no RAM). `SPRITE_WORDS` (the last picture) is the picture's own copy (`CMD_COPY` into a slot).

## Where it stands

- `shader_vm/src/gpu.h`, `shader_vm/gpu.shader`, `harness/apps/rvc_backend11.cpp`: the device side done in the branch
  (`gpu.h` 2,149 lines to 1,436, the stream passes and switches gone), not built yet.
- `programs/common/gpu_uploads.h` written (shelves in rows 2048 to 4095, one state a list maker, text and sprite slot
  tables at word 15, sprite records by `FRAGMENT_SPRITE` uploads); called by gles.c (kept commands given their own
  textures back first: `kept_texture`), Nano-X's driver (`flush_last`), nxweb's strips (`strip_job`) and
  `programs/common/gpu.h` (`gpu_submit_as`). Quake III, Half-Life and Jedi Outcast take it through gles.c.
- The device side compiles (30 stages on WARP), and with the old programs draws the six snapshots' pictures as before:
  Doom, Quake, Quake III, glxgears, Half-Life, the desktop, 0 pixels differ, 0 magenta.
- Every program rebuilt (gles.c's users, Nano-X, nxweb, ClassiCube, OpenTTD, the bare-metal ones): `test_all` 271 of 272
  (cc-bytes moved by libgles.a, set to cf92a103), and every graphics program of the image captured under `--cpu` with
  nothing magenta of the device's (32 of them, OpenTTD's title through its sprite uploads; nxpaint's palette and
  demo-blit's text are magenta of their own). OpenTTD's sprites went per list after all, with the duplicates found by a
  hash (a record's pixels, kind, size, remap, palette and tables): no persistent slots, so the sprite cache needs no hook.
- The holodeck draws from the front end's targets (written, compiled in Unity's shaders by `tools/unity_fxc.py`, not run).
- Code: `gpu_draw.h` 692 lines of code against the drawing's 1,391 of `gpu.h` at 1dff0e7d (2.01 times less), `#if` 3
  against 73 there; `gpu.shader` 207 against 298, its `GPU_` defines and switches 14 against 53.
- The card (RTX 5090, `RVC_PASS_PROF` "gpu draw" a round, gpubench, the mean of two 8 s runs each way), the RAM path
  (1dff0e7d with `GPU_NO_STREAMS`, gpubench without uploads) against the VRAM alone: words smooth 0.308 to 0.207 ms, indexed
  smooth 0.320 to 0.207, three bytes smooth 0.445 to 0.207, compact with a light map 0.532 to 0.214; but colour 0.181 to
  0.209, words nearest 0.182 to 0.208, indexed nearest 0.175 to 0.207. A fixed 0.21 ms whatever the mode: the front end
  and the draw IDs wrote all 196,608 texels every frame, now into eight targets of 16 bytes, for lists of 50 vertices.
  Both now draw the rows the list's vertices take alone (`gpu_rows_vertex`, from the last command's end), and the frame row
  says how many (texel 4 this frame's, 0 with no list; texel 5 the last list's, the holodeck's).
