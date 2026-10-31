# ClassiCube

ClassiCube, the open client for Minecraft Classic, runs in the Linux image as a Nano-X client:
a 64 x 64 x 64 world it generates at start, to walk, dig and build in. Everything on the screen
is drawn by the machine's GPU. A frame of it is almost all drawing: the world is chunks of quads
that stay in GPU memory, and the CPU works when a block changes.

    wsl -- bash linux/nommu/build.sh nanox/build.sh        (once)
    wsl -- bash linux/nommu/build.sh classicube/build.sh   (6 s; the first time it also fetches)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                     (then a new shell snapshot)

In the guest: `nano-X -p & classicube` at the console, or ClassiCube in the Start menu's Games.
It starts in single player. W, A, S and D walk, Space jumps, the left button digs and the right
one builds; the pointer's travel over the window turns the view, and so do the arrow keys, since
the pointer is not held in the window. Escape is the game's menu. Its textures are the game's own
(`misc/cc_textures.zip` of its repository), so the build fetches nothing but the source and,
once, the sounds.

Lines typed at the terminal it was started from drive it, which is how a test reaches it
through the machine's serial console:

    down KEY / up KEY     a key or button, by the game's names (W, Space, Right, LeftMouse, RightMouse)
    turn DX DY            the pointer's travel, in pixels
    point X Y             the pointer's place in the window
    hold                  the frame after next stays ("classicube: holding frame N")
    anything else         said in chat (the game's own commands start with /)

## What the build is made of (`linux/classicube`)

- `build.sh` fetches the source at a fixed commit, applies `classicube.patch` and compiles every
  file of the game with `PLAT_SHADEREMU`: each leaves itself out by the platform's defines. The
  16 files a profile is full of, and the worker's `Builder.c`, are `HOT` (`-O3 -fno-tree-vectorize
  -fsched-pressure -funroll-loops`), and the build takes `classicube.profile`'s counts of two new
  worlds (`CLASSICUBE_PROFILE=count` makes the program that counts: `docs/games/tdawn.md`, "What the
  compiler is told"). `CC_API` and `CC_VAR` are empty: its globals are hidden like the rest.
- `Graphics_ShaderEmu.c` is the game's graphics backend, written on the GPU device itself
  (`docs/machine/gpu.md`), not on the OpenGL library: command lists, matrix blocks and GPU memory are
  its own.
- `Window_ShaderEmu.c` is the Nano-X window, the keys and the pointer, and the terminal's lines.
- `Math_ShaderEmu.c` is sine and cosine in floats: the game's own work in doubles.
- `store_zip.py` rewrites the texture pack with its entries stored and the pictures the game reads
  at its start in one pack of PNGs, its first entry, which `png_shaderemu.c` decodes on the worker
  cores through libmedia while the game starts ("The texture pack's pictures", below).
- `classicube.patch` (`git diff` of the tree at `build.sh`'s commit): the platform in `Core.h`
  (low-memory build, no launcher, no FreeType, the sound card as its audio backend), a vertex colour as the device has
  them, the map generator's noise in floats, one chunk built a frame, the chunk builder's
  short cuts (below), water's and lava's animation at 5 and 2.5 pictures a second, random
  block ticks at a quarter of the rate, the map's borders in quads of 16 blocks, a view
  distance of 128, a seed from `CLASSICUBE_SEED`, and the speed key (Shift) at twice the walk
  where the game's default is eleven times (`hacks-speedmultiplier` 1, not 10).

## Meshes from worker cores

On a machine with worker cores (`docs/machine/multicore.md`) the meshes are all theirs, and the first
core builds none:

- `Builder.c` is compiled again for each of seven copies (`build.sh`: `-DSE_SECOND`, its four public
  names renamed; `SE_COPIES` in `MapRenderer.h`). A copy has every buffer of the first as its own, takes
  up to two chunks at a time and builds them into memory of its own; it makes no vertex buffer and writes
  nothing of the chunks'. A copy is a call's, on whichever worker is idle.
- Each frame the first core gives the nearest chunks that wait for a mesh to the calls that have none
  (`SE_Mesh_Post` in `MapRenderer.c`), and makes what a worker has finished the chunks' own (`SE_Mesh_Take`):
  the parts' counts, and the vertices into GPU memory as any chunk's are (`Gfx_SE_UnlockPacked`). Two chunks
  a job, so that the nearest are drawn soon and every worker has some.
- The copies share no variable they write. The game's `Drawer` (a block's corners, written for every block
  drawn) is not used by the builder's own faces (below).
- A chunk that is made again (a block changed) is a worker's too, and is drawn by its old mesh until the new
  one is there: the old is let go and the new put in its place in one frame. A chunk has a count of the times
  it was asked to be made again (`made`): a mesh begun before the last of them is thrown away and begun again,
  and whatever frees the chunks or the blocks waits for the workers first (`MapRenderer_SE_Settle`).
- The light's heights of the whole map are made by the workers before the first mesh, rows of z each
  (`SE_Light`, `ClassicLighting_SE_Rows`): a row is whole 16 bytes. After that a changed column is the first
  core's, on the first core for the worker's chunks too (`SE_Mesh_Post`): both builder copies calling
  `LightHint` could store into the same 16 bytes. A light height asked for on core 0 while the workers still
  make the rows (`SE_Rows`) waits for them (`ClassicLighting_GetLightHeight`).
- A chunk too large for a worker's buffer (16,384 vertices) is the first core's (`se_own`), and with no workers
  (`CLASSICUBE_MESH=inline`) all are, one a frame.
- The build gives every variable its own 16 bytes, as Quake's does, and the game touches all of its data once
  before the first job (`mcw_touch`), or the worker would stop at each page of its own buffers.

The workers are asked through the second layer of the library (`programs/mc/mcwx.h`): the world's height map,
strata and surface by `mcw_for` (rows of z, up to 15 workers and the first core; `SE_GenShare` in `Generator.c`,
`SE_SurfaceRows`; the lowest height is read off the finished map), the light's heights and the meshes by
`mcw_call`, looked at with `mcw_ready` and taken with `mcw_join`. The rest of the world's making draws from one
run of random numbers and stays in order. `NotchyGen_FillOblateSpheroid` is in whole numbers by rows, the random
numbers inline (the same numbers), one cosine of phi a cave step: the same world.

## How it is drawn

- **A chunk is packed vertices** (`VERTEX_PACKED`): a word a vertex, bytes of x, y and z in
  eighths of a block from the chunk's corner, with the texture coordinates a word beside it:
  32 bytes a quad. Each chunk has a matrix block that puts it in its place. The colour of a
  vertex (a face's shade, in sun or in shadow) is a tag into one table that all chunks share.
- **The builder writes those vertices itself** (`SE_RenderBlock` in the patch's `Builder.c`,
  in place of the game's `NormalBuilder_RenderBlock` and `Drawer.c`): no float is made to be
  converted. What a kind of block's faces come to (its corners in eighths, its texture
  coordinates in 1,024ths, with the game's own float arithmetic and the backend's rounding)
  is worked out once for each chunk that has the kind; a block is then integers added to
  them, and a run of blocks whole tiles added to u. `Gfx_SE_UnlockPacked` moves the bytes to
  count from the mesh's origin and puts each quad's tag in. The game's builder for smooth
  lighting still writes floats.
- **The terrain's textures are four across.** A packed vertex has 12 bits of u in 1,024ths,
  four repeats, and a run of 16 equal blocks is one quad that repeats its tile 16 times. So
  the atlas the chunks read is the game's (a strip 16 texels wide) written four times side
  by side, and u is a quarter of the game's.
- **Other buffers are compact vertices**, a texel each: with the command's colour when all of a
  buffer's vertices have one, with tags and a table of the buffer's own colours otherwise. A
  quad's texture coordinates are moved to start in the texture's first repeat.
- **A buffer filled and drawn again and again in one frame lives in the frame's set** (the 2D
  screen's quads, the models); one drawn frame after frame as it is gets GPU memory of its
  own on its second frame, and costs nothing after.
- **Passes**: what tests depth is pass 0, or pass 1 when it blends (water, which the game
  draws twice, depth first: here that first draw is left out, since pass 1 writes no depth).
  Everything drawn without depth (the 2D screen, the block in the hand) is pass 5, blended,
  so that it keeps its order. The alpha test is a key: a texel with no alpha becomes wholly
  transparent black when its texture is copied, and pass 0 leaves those out.
- **GPU memory** is the 3.3 MB a GL program has and, while the desktop is layers, the
  display's own framebuffer (3.9 MB), handed out in spans; a span given back is free two
  frames later, so that the list being shown never reads freed memory.
- **Faces drawn once.** The device has no culling (`docs/machine/gpu.md`): `Gfx_SetFaceCulling` does nothing,
  and every face is seen from both sides. Of two whole transparent-thick blocks of one kind only the face of
  the one on the greater side is made (`Block_CalcCulling`). A sprite (a flower, a sapling) is two crossed
  planes of two quads each, the second the first mirrored: of each plane the walk draws one quad, by the
  chunk's sides towards the camera as the game chose (`RenderNormalBatch`).

Not as the game draws elsewhere: no fog (the far chunks end at a line), clouds that stand
still, no lines (the block outline is the game's quads, so it is there), nearest texels, and
no smooth lighting.

## Speed

`game_par.py classicube --calibrated` (31 October 2026): seed 7, block physics off, walking and jumping into
a hill, five windows of 2 s from `classicube: steady`.

| Frames a second | Passes a frame | Core 0 a frame | All cores a frame | The busiest worker |
|---|---|---|---|---|
| 175.7 | 3.52 | 12.9k | 70.7k | 17.6k |

The frame's worker makes its list in about 13.6 thousand instructions, and core 0 waits about 2.9 thousand for
it (`ccstat:`). The instructions of a frame on all cores include the music's decoding. In the same model, from
the machine's start to `classicube: steady` is 13.2 s of guest time, 40 million instructions on core 0 and 138
million on the workers.

A block put at the top of a deep shaft changes the light of every chunk under it, and water flowing after a dig goes
on changing blocks, a chunk each time. Those are the workers'.

What makes a chunk's mesh short (`-DSE_COUNT` gives a chunk's instructions by phase, in the `ccstat:` lines):

- A row of 18 blocks along x is read as four words of the world and two bytes, written into the chunk as words, and
  becomes two masks of a bit a block as it is read: the full opaque blocks and those not drawn (`Builder_ReadRows`;
  four blocks of one kind are one look at the tables).
- From the masks of a row and the four rows round it: which blocks to look at at all (not air, not a full opaque block
  among six), and for each of those which faces a full opaque neighbour hides, without the tables (`Builder_RowTodo`,
  `Builder_Covered`). Outside the world counts as full opaque where the game shows no face towards it (under the world,
  beside it below the sides' level). The blocks with a face to draw are noted for the drawing loop, which asks nothing
  of the others.
- The bookkeeping walks the four atlases a map uses instead of all 512.
- Not done: a block whose only open face an earlier block's run has taken is still looked at; a mask of what runs have
  taken would pass over them.

### What keeps a frame short

- **No copy by a call in a draw.** `DrawQuads` keeps its six words of state, and `MatrixBlock` its sixteen, by word copies
  in place.
- **A draw's state is worked out when it changes** (`state_serial`, `DeriveState`, `Emit` in `Graphics_ShaderEmu.c`). A new
  kind of state must bump `state_serial`, and anything that makes a mesh again goes through `MeshMade`.
- **A chunk's matrix block** is rows made once a matrix (`ChunkRows`) and three words a chunk.
- **Loops over entities start at `Entities_SE_First`**: alone, the player is slot 255 of 256. The translucent depth-only
  walk is left out (the device draws nothing in it), `ResetPartFlags` resets the atlases in use, and the terminal is asked
  for a line 20 times a second, not every frame.
- **Particles are removed in one pass** (`Terrain_Tick`, `Terrain_RemoveFirst`).
- **A walk of the chunks leaves its commands in the set, and they stay.** The map's renderer counts what would change the
  game's two walks (`MapRenderer_RenderNormal`, `MapRenderer_RenderTranslucent`; `se_walk_serial`: the list of chunks in view
  or its order, the faces towards the camera, a mesh made or let go, the atlases, the camera under water or not) and the
  backend notes a walk as it is made (`Gfx_SE_SegmentBegin`, `Gfx_SE_SegmentEnd`): where its commands are in the set, what they
  come to, the buffers drawn. While the count stands, no buffer or texture was made again (`kept_epoch`) and the walk would begin
  at the same command and vertex, the commands are left where they are in that set and the walk is not made. Each of the two sets
  has its own note, and one that a frame did not use is forgotten. `CLASSICUBE_WALK=every` makes them every frame; the `ccstat:`
  lines say how many walks were made and how many kept.
- **A chunk's matrix block has a place of its own in each set** (`KEPT_BLOCKS`, a slot a packed mesh), and the matrices have a
  number that stays while they are what they were (`MatricesLoaded`: the camera's, compared word for word when it is loaded; any
  other load has a number never used before). A block written for the camera's number is right until the camera moves: standing,
  nothing of the chunks is written at all. The 2D screen's and the held block's blocks are the frame's.
- **Which chunks are in view is asked without going through them** when no chunk waits for a mesh: a plane's products with the
  grid's x, y and z are made once (`SE_Frustum`), a chunk's test is three sums a plane in the order `Frustum_TestSphere` makes them
  (the same floats), in the map's own order, and when every chunk is in or out as it was the list of those drawn stands
  (`SE_SameView`). With the camera where it was nothing is asked. Every chunk in every list, with no test, is dearer: every chunk's
  block is then written when the camera moves.
- **The frame's time is the machine's clock word** (`Stopwatch_Measure` in `Platform_Posix.c`): whole milliseconds, no system call
  and no division of doubles a frame.
- **The block outline is made only for a new box** (`SelOutlineRenderer.c`): the four corners it is made of are compared with those
  it was last made of. **So is the held block** (`BlockModel_BuildParts` in `Model.c`): the same block, colours and textures, with no
  other model's vertices put in the shared buffer since (`se_locks`), is drawn from the vertices there. **A dynamic buffer filled as it
  was keeps its mesh** (`SameFill` in `Graphics_ShaderEmu.c`): a buffer of 16 vertices or more keeps a copy of what its mesh was made of,
  and a fill that is word for word the same makes nothing.
- **The frame's list is not waited for.** `Gfx_EndFrame` submits and goes on (`docs/machine/boot.md`, "Passes that end early"). A
  frame writes its own set of commands, matrices and arena, which the list being drawn never reads; the two things that are
  written in place settle for it first (`Settle`): a buffer's own span when the frame before drew from it (`SpanAlloc`; a dynamic
  buffer goes into the frame's arena instead while the list is still out), and a texture's texels. An animated tile's new texels
  (water, lava) are kept and written when the frame ends, before its own list is submitted (`Later_Flush`): that list is the first
  that is to show them. A tile is 1 KB and its four copies across 4 KB more, most of what a pass keeps of new stores.

Not done: a chunk's block is 28 words written whenever the camera moves, 25 of them the same for every chunk: the device reads a
block where the command says, so one block for all chunks wants an origin in the command (a change to the GPU's shader, its model and
Unity's copy). The vertices of a worker's mesh could be the device's when the first core gets them. The seven copies of the builder are
3.3 MB of variables that the first core touches at the start.

`ccstat:` lines, every five seconds, give frames, instructions a frame (core 0's), draws, quads, and the texels and vertices made for
the device.

    CLASSICUBE_SEED=N        the same world every time
    CLASSICUBE_HOLD=N        keep frame N on the screen and stop there
    CLASSICUBE_HOLD_LATE=1   with CLASSICUBE_HOLD: the frame stays while the next is made ready, then the game stops
    CLASSICUBE_SWAP=wait     every list waited for
    CLASSICUBE_STATS_MS=N    how often the ccstat: lines come (5000; 0: never)
    CLASSICUBE_SIZE=WxH      the window's size (854x480)
    CLASSICUBE_MESH=inline   every mesh by the first core, in one order every time
    CLASSICUBE_MESH_WORKERS=N   at most N workers for the meshes (7)
    CLASSICUBE_WALK=every    the chunks' commands made anew every frame
    CLASSICUBE_MESH_SUM=1    a ccmesh: line a chunk mesh made: origin, vertices, two sums
    CLASSICUBE_TEXTURE_SUM=1 a cctex: line a texture made
    CLASSICUBE_DEFINES=-DSE_COUNT   (to build.sh) a chunk's instructions by phase, in the ccstat: lines
    CLASSICUBE_RENDER=inline the rest of a frame's list by core 0 too, not a worker
    CLASSICUBE_CHECK_KEPT=1  the outline and the held block are made again when kept, and compared (cccheck: lines)
    CLASSICUBE_OUT=DIR       (to build.sh) the files go to DIR, a private image's overlay, not the shared root

## The texture pack's pictures

`store_zip.py` puts the 19 pictures the start reads (not `touch.png`) into one picture, rows of 256 of the game's own pixel words
(blue, green, red, alpha: libmedia's copy, no conversion), each picture's words from a page of their own, and writes it as 8 PNGs one
after another, every row filtered with None (smaller here than with Sub or Up, and quicker to undo). Each PNG has 16 segments: the first
one stored row, which mcwx gives the first core, and 15 alike for the game's 15 workers. The pack is the zip's first entry; each picture's
own entry is a stub (`SEPACKED`, the pack's name, where its words begin, its width and height).

At `Game_Setup`'s start `SE_PackEarly` opens the workers (as the world's generator would, later), reads the pack from
`texpacks/default.zip`, maps its pages, decodes the first PNG itself and leaves the other seven to a thread at nice 8 (at the game's own
priority each look of its wait for the workers would end a pass the game could have). The zip's first entry joins it (`SE_PackEntry`), and
a stub's picture is its own pages of the pack (`SE_PackPicture`), unmapped when the game frees it (`Mem_Free` asks `SE_PackFree`). Another zip
with a pack is decoded where its entry is read. No other code of the game's asks the workers for anything before that join. With
`CLASSICUBE_MESH=inline` (no workers) the first core decodes the pack where its entry is read.

`Png_Decode` hands any other PNG to `SE_PngDecode` (`png_shaderemu.c`), decoded by libmedia (`docs/system/media.md`) on the worker cores into
the game's pixel words. `build.sh` writes both the pack and the raw form.

## The frame's list on a worker

A frame is made in two halves. Core 0 ticks, reads the input, moves the camera, picks the block looked at and keeps what the frame is seen from
(`struct SEFrame` in `Game.h`: the view, the camera's place, bobbing and tilt, pitch and yaw, the block picked, the held block's colour, the
time); then it waits for the list of the frame before (`Gfx_SE_Settle`: `mcw_join` and the submit that is `Gfx_EndFrame`'s), begins the new list
(the clear, the matrices, the frustum, the particles; meshes the mesh workers finished are taken and new ones given out, which `mcw_call` wants
of core 0: `MapRenderer_SE_Between`), and gives the rest to a worker by `mcw_call` (`SE_FrameRest` in `Game.c`): the animated water and lava, the
sky and clouds, the map's walk, its sides and edges, the outline, the held block, the 2D screen, and the animated tiles' texels at the list's end.
Its next frame's ticks and input run beside it, so a frame is shown a frame later.

What the worker reads that core 0 goes on changing it reads from the kept frame (`SE_Frame`): `MapRenderer.c`, `EnvRenderer.c`,
`SelOutlineRenderer.c`, `HeldBlockRenderer.c`. What both may change, core 0 waits for the worker before it changes (`Gfx_SE_Own`): textures and
buffers (`Gfx_UpdateTexture`, `Gfx_DeleteTexture`, the buffers' making, locks and deletes, the viewport), the C library's heap (`Mem_*` in
`_PlatformBase.h`: one core at a time in it; the game's other thread, HTTP's, does not join the worker but waits on a lock the game's thread holds
while a list is out, `Gfx_SE_HeapEnter`), a block changed (`Game_UpdateBlock`), the chunks made again (`ChunkInfo_Refresh` and the map's events), the
held block's animations, and every key, button, pointer move or terminal line (`Window_ShaderEmu.c`). The ticks only count the animated tiles; the
worker makes them (`Animations_SE_Catchup`). The chat's line of a texture pack's download asks the HTTP thread's two words without its lock on the
worker (`Http_GetCurrent`): a lock taken on a worker is a change `RVC_RACES` undoes.

The rest is a worker's while the player is alone in the world and seen from the eye (the player's model is then not drawn: core 0 makes its
animation, `AnimatedComp_GetCurrent`); with others, from behind, without workers, or `CLASSICUBE_RENDER=inline`, core 0 makes the same list itself, in
the same order. Its instructions are spread over all the workers, since `mcw_call` takes the next idle one.

## The holodeck's seat

From a holodeck's seat (`docs/world/holodeck.md`, "Played from the seat"): the pick starts at the hand, the chunks are those round the head, a box round
the eye stands for the clear colour, the view does not tilt with the walk's bobbing, and the game says its eye's height for the room's scale
(`seglWorldEye`). The held block's hand is the frame's (`SEFrame.hand`), worked out on core 0. The eye said is the player's real one, so a block is about
a metre, and the block in a sitter's hand is 4 cm in the room whatever the world's size (`SE_SeatHeld`, `heldScale`). In a headset the view is lifted over
blocks only when they are small (`SE_SeatLift`); the right trigger breaks by the right hand's beam and the left one places by the left hand's; a desktop's
sitter breaks by the left button and places by the right one at the view's middle (`docs/world/holodeck.md`, "The world's size" and "Played from the
seat"). `CLASSICUBE_SEAT=1` prints a `ccblock:` line for each block changed, and the seat's line has the block's size in metres and the lift; the
`classicube-seat-*` cases of `tools/test_all.py` break, place and change the block in hand by the host's words.

## Sound

`Audio_ShaderEmu.c` is the game's audio backend for the sound card (`docs/machine/sound.md`). Its sounds are Mojang's, as the game's own builds fetch them:
`sound/make_sound.py` (run by `build.sh` once) takes the dig and step WAVs that `Resources.c` names from Mojang's server, checks their SHA-1s, and writes
`classicube-sound.pak`: 59 sounds, mono, ADPCM with a checkpoint every 32 samples (`sound/adpcm_encode.c`). A dig or a step is a voice pointed at its sound in
the ROM; nothing is decoded while the game runs.

The music is one tune, `calm3`, made an MP3 at the image's build (ffmpeg, 128 kbit/s, `classicube-music.mp3`). libmedia decodes it at half its rate in pieces
of a second on up to 6 worker jobs, each into its own staging buffer and then into a 1 MB ring of sound memory (11.9 s at 22,050 Hz) that a stream voice
plays; core 0 only joins finished jobs and moves the voice's written count. Pieces are handed out while the ring has room, and the tune plays again after
the options' delay. The options' music volume works: 0 opens nothing.

- `CLASSICUBE_SOUND_LOG=1` prints each sound played (`ccsound:`) and every eighth piece of the tune with the frames written and heard and how often the voice
  ran dry (`ccmusic:`).
- The test set's `classicube-sound`: the pack is found, a dig plays, and the tune is decoded to 16 pieces.

## Checking it

`tools/test_all.py`'s `classicube-*` cases:

- `classicube-meshes`, `classicube-meshes-workers`: `CLASSICUBE_MESH_SUM=1 CLASSICUBE_SEED=7`, 52 meshes (68,964 vertices) at the start, digest `7f763849`
  line for line with `CLASSICUBE_MESH=inline`, and `1f9d723c` as a set by the second sum with the workers (that sum has each vertex's colour for its tag: the tags
  go by the order chunks are made in).
- `classicube-pack`: `CLASSICUBE_TEXTURE_SUM=1`, the 30 pictures' `cctex:` digest `43d1cb6b` (the lines of text, 26 high, are counted, not summed: their number goes by
  how fast it starts).
- `classicube-kept`: `CLASSICUBE_CHECK_KEPT=1` says the outline and the held block kept, 0 of them wrong.
- `classicube-seat-*`, `classicube-sound`: above.

By hand:

- A change to how it builds chunks: the meshes' bytes must stay what they were, after a block dug too. With the first core, `turn 0 300` and at a later `ccstat:`
  line `down LeftMouse` and `up LeftMouse` typed together (the turn must have had a frame), which makes two or three chunks again. Switch block physics off for it
  (`echo singleplayerphysics=false >> /tmp/classicube/options.txt` before the game): dirt turns to grass by the clock, and two builds' worlds part after a dig.
- `RVC_RACES` walking, jumping and digging: 0 undone. Reads as they were are the guard's look at the call, the HUD's at the player's place, the tiles' count,
  `team_job`'s look at the round word.
- The GPU against its model: hold a frame, pass `--save-state` and `--gpu-capture`, and run `python tools\gpu_reference.py SNAP BMP --size 854x480`: pixels away from
  triangle edges agree.
- A change to how it draws: hold the same frame of the same seed before and after and compare the GPU's targets. Two runs of one build give the same picture
  (`rvc_harness --cpu`). A held frame's window is the top left 854 x 480 of a `--gpu-capture`: outside it two builds may differ. Digging held down is by the clock too:
  dig by single presses.
- A change to what the backend writes when: `CLASSICUBE_HOLD=1500 CLASSICUBE_HOLD_LATE=1` with `--gpu-capture`, once with `CLASSICUBE_SWAP=wait` and once without: the
  pictures must be the same. A plain hold stops the game before it writes anything for the next frame, and cannot show a store made too early.
- To see where instructions go, snapshot a run at `classicube: steady` and resume it with `--pc-log` (`tools\pc_profile.py`, `classicube=classicube.nm`), or `python
  tools/prof.py` (`docs/machine/profiling.md`).

## Not done

- Playing it by hand with the window's keys and pointer; the terminal's lines and the host's words are what the tests use. The world's beams are a pointer, not a mouse
  that is held: turning by them wants a look.
- Music: one tune of Mojang's, not the game's whole list.
- Fog, and clouds that move.
- A world larger than 64 x 64 x 64 (the low-memory build's), and one kept from run to run: a saved map goes to `/tmp`.
