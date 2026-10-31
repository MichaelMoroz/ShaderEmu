# Quake

Quake (1996) runs in the Linux image: id Software's released source, GLQuake, as a Nano-X
client. Everything on the screen is drawn by the machine's GPU; there is no software renderer
in the build. The machine's float instructions (`docs/machine/fpu.md`) were added for it.

    wsl -- bash linux/nommu/build.sh nanox/build.sh     (once)
    wsl -- bash linux/nommu/build.sh quake/build.sh     (3 s; the first time it also fetches)
    wsl -- bash linux/nommu/root.sh
    pwsh tools\make_image.ps1                                  (then a new shell snapshot)

In the guest: `nano-X -p & quake` at the console, or Quake in the Start menu's Games. With no
arguments it plays its three demos in turn, as Quake does, and Escape brings its menu up, where
New Game is; the Start menu starts it with the menu up already (`quake +menu_main`). `quake +map
e1m1` starts the first level. The data is the shareware episode's `pak0.pak` (version 1.06), which
the build fetches and the repository does not keep.

Lines typed at the terminal it was started from are console commands (`map e1m2`,
`timedemo demo1`, `hold`), which is also how a test drives it through the machine's serial
console: a test that types console commands runs `quake` in the foreground. The window's keys go
to the game as Quake's own (`vid_shaderemu.c`); in the game the left button held drags the view
round and the right one fires (`docs/machine/input.md`), and `in_mouse 1` has the pointer's travel
over the window turn the view too.

## What the build is made of (`linux/quake`)

- `build.sh` fetches the source at a fixed commit, applies `quake.patch`, adds the files
  below and compiles GLQuake's file list without its x86 assembly, with the null CD driver and
  the game's own network drivers (the loopback and UDP: `docs/system/lan.md`). Floats are the
  machine's instructions, constants are single precision. The build is `-O3 -fno-tree-vectorize
  -fsched-pressure -funroll-loops` with GCC's inlining raised, and with `quake.profile`'s counts
  (`QUAKE_PROFILE=count` makes the program that counts, `off` leaves them out: `docs/games/tdawn.md`,
  "What the compiler is told").
- `qgl.c` is the OpenGL GLQuake calls (`include/GL/gl.h` renames each call to `qgl...`): the
  1.1 calls it uses, glBegin and floats, turned into what `programs/linux/gles.c` (its float
  build) draws. It also keeps the textures' pool, the models' poses and their light tables.
- `world_shaderemu.c` is the level and the models (below).
- `mesh_shaderemu.c` stands in for `gl_mesh.c`: a model's triangles as they are, where GLQuake
  searched each model for long strips.
- `fmath.h`, `fmath_shaderemu.c`: sine, cosine, tangent and arctangent in floats. The source
  calls C's double functions, and the C library's float ones work in doubles inside.
- `vid_shaderemu.c` is the window, the frame and the input; `sys_shaderemu.c` the system;
  `server_shaderemu.c` the server's frame and the workers' calls, `scene_shaderemu.c` the
  picture on a worker, `gpu_shaderemu.c` gles.c with what a draw made again asks of it (below).
- `snd_shaderemu.c` is the sound (below), in place of the game's mixer.
- `quake.patch` is `git diff -- WinQuake` of the tree at `build.sh`'s commit.

## Sound

The machine's sound card plays it (`docs/machine/sound.md`): a sound is a voice pointed at its WAV
where the pak file lies in the ROM, with the WAV's own loop point, and nothing is loaded or
mixed by the CPU. The game's rules stay: which of eight channels a thing's sound takes, and
how loud it is in each ear by distance and side, worked out once a frame.

- A level's own sounds (torches, hums) are loops with a place. The 18 loudest that are heard
  have a voice each; one that is out of hearing gives its voice up.
- Water and sky are two more voices, as loud as the viewer's leaf says.
- Its level is half the game's own, so a fight's mix rarely runs into the limit. `volume` is the
  game's setting.
- No music: the game's was a CD's tracks.
- To check it: `--fixed-dt 0.004 --sound-capture F.wav --save-state F.snap` on a timedemo and
  `python tools\sound_reference.py F.wav F.snap --rom build\images\nommu\rootfs.bin`: every sample
  must be the model's.

## How it is drawn

- **Textures stay palette indices.** GLQuake's 3dfx path for 8-bit textures is the one used,
  without its rescaling to powers of two and without mipmaps: the GPU looks each texel up in
  the palette, and index 255 is a hole where the texture has any.
- **The level is kept in GPU memory** (`seglKeep`, `docs/machine/gpu.md`), as Doom's is: its polygons
  are made into quads when it loads, sorted by texture, and once more sorted by light map,
  and a frame says which runs of them to draw. The light maps are a second draw that
  multiplies (pass 3) with the light map as a texture of grey words. Light maps are placed by
  looking only where a block's height changes and never in a block that failed for a smaller size.
- **What is in view** comes from the level's own table of which leaves see which (its PVS):
  the leaves the viewer's leaf sees are listed when the viewer enters a leaf and tested
  against the view's four planes in integers. With the light maps and water the workers', only
  the things standing in those leaves are listed and tested ("The other workers, and draws made
  again").
- **The sky** is two kept commands over the same vertices, their texture laid on the picture
  (`FRAGMENT_LAID`). Water is kept too, its ripple written by workers (the same section); water
  one can see through is drawn a polygon at a time with the game's clock.
- **Water's two faces.** A level's water, slime, lava and teleporters are two faces on one plane,
  one for each side, and the GPU culls nothing. One face of a pair is kept (`water_twins` in
  `world_shaderemu.c`, found by plane, side, texture and the sum of its corners), and the other's
  leaves ripple it: the load line says how many faces were left out.
- **A model's pose is the file's own bytes.** A vertex of a model is four bytes (x, y, z and
  which of 162 normals); a pose's are copied into GPU memory as they are and drawn as packed
  vertices, with the model's texture coordinates in a buffer beside them. Its shading is the
  same vertices drawn again, multiplying, each coloured from a table of the light at every
  normal (`VERTEX_TABLE`): a table a light and a turn, 128 of them kept.
- **A particle is one vertex** (`VERTEX_POINTS`): the GPU makes GLQuake's triangle of it, and
  its size. Its colour is a palette index: a 16 x 16 texture holds every index once.
- **A flash** is the glow GLQuake draws by default (`gl_flashblend 1`), a square of a round
  picture, added. Lighting the walls with it instead would make their light maps again each frame.
- **The 2D screen** (status bar, console, menus) is rectangles of textures in the one blended
  pass with no depth (`seglSprite`), so that it keeps the order it is drawn in. It is runs of
  sprites the program writes itself (`qglRect`, `qglText`, `qglPicture`; `seglSpriteSpace`).
- **The frame a level loads under is flat.** The loading plaque's frame stays while the next level
  loads, on black (`SCR_UpdateScreen` leaves the view out under `scr_drawloading`), and the frame
  says it is a flat screen (`seglWorldFlat`, `vid_shaderemu.c`), as a menu does. So does a level's
  end (`cl.intermission`).
- **GPU memory is a cache.** Every texture is kept in ordinary memory as the GPU will read
  it; GPU memory holds the ones being drawn, the level, and the poses and tables, and what
  was drawn longest ago makes room (a pose first). It is 7.2 MB: the 3.3 MB a GL program has
  and the display's own framebuffer, which nothing uses while the desktop is layers
  (`seglMemorySpare`).

Not as GLQuake draws: no model shadows, no wobble of the world seen from under water, no
depth trick for the weapon (it can poke into a wall), nearest texels where a texture does not ask
otherwise, and a model's light in steps of 1/32.

## Smooth textures

The game asks for `GL_LINEAR` of the level's textures, its light maps, the models' skins, the
sky and its pictures, and for `GL_NEAREST` of the console's letters, and is given both: the GPU
weighs the four texels round a point for a texture so marked (`docs/machine/gpu.md`, fragment flag
`0x400`; `qglTexParameterf` in `qgl.c`). There are no smaller copies of a texture for far walls
(mipmaps), so those shimmer. `QUAKE_FILTER=nearest` is every texture's nearest texel.

## Speed

Calibrated, 31 October 2026 (`game_par.py GAME --calibrated`):

| | Frames a second | Passes a frame | The first core a frame | All cores a frame |
|---|---|---|---|---|
| e1m5 played, standing where it starts (`quake`) | 93.9 | 7.26 | 23.9k | 133.7k |
| `+timedemo demo1`, 969 frames, no server (`quake-timedemo`) | 109.3 | 5.79 | 37.2k | 107.3k |

On e1m5 the server made 2,329 frames to the client's 4,698, about 101 thousand instructions each, and the drawing worker about 19 thousand a frame. The client is at its 10 ms floor.

Play is what counts, never the timedemo alone: the timedemo has no server. `quakestat:` lines, every
five seconds (`QUAKE_STATS_MS=N`; `QUAKE_STATS_FRAMES=N` is every N frames, the same frames each run,
so two builds compare line by line), say where a frame went, how many were drawn by a worker, and the
server's frame in ms of the clock; a level's first line says how many instructions it took from the
start. The game slows down rather than skipping when a frame is longer than a tenth of a second, as
Quake always has. A frame under four passes waits for the fourth: a frame is refused under 10 ms
(`Host_FilterTime`, 100 a second where the game has 72), and the main loop waits for the clock by ending
the pass.

    QUAKE_HOLD=N        keep frame N on the screen and stop there ("quake: holding frame N")
    QUAKE_STATS_MS=N    how often the quakestat: lines come (5000; 0: never)
    hold                the console command: the frame after next stays
    -nokeep             draw the level a polygon at a time
    QUAKE_DEFINES=-DSE_COUNT    (to build.sh) counts of the server's work in the quakestat: lines

What else keeps a frame short on the first core:

- Polygons drawn one after another (a door's faces and their light maps, water) are written
  straight into the frame as the two triangles the GPU makes of a quad (`qglQuadsRoom`): room for
  64 is taken at once and the rest given back when anything is set.
- An entity's matrix is made of the world's in one go (`qglEntity`), sum for sum what gles.c's
  translate, rotates and scale make (its sine of 90 degrees is not 1: a rotation cannot be left
  out or simplified loosely). The view's matrix likewise (`qglView`).
- Particles are written where the GPU reads them, and `rand()` is the program's own copy of the
  library's (the same numbers, `sys_shaderemu.c`), inline in `r_part.c`.
- What nobody reads is not made: the nodes' marks (`R_MarkLeaves`), the palette's ramps
  (`V_UpdatePalette`), the view model's own light, `bitcounts`.
- The client looks only at entities that had an update (`se_entity_live`), brush models only at the
  light maps they used, and gles.c is told only state that changes. The message's readers are inline
  in `cl_parse.c` and `cl_tent.c` (`SE_MSG_INLINE`), and a notify line is drawn up to its trailing
  spaces (`se_line_length`).
- Loading: the kept level's surfaces are sorted once; nothing is byte-swapped; the Hunk clears memory
  when it is taken and not at all under a file read into it (`Hunk_AllocUnset`); the palette's gamma
  is a table.
- The terminal is asked for a typed line four times a second, and `R_AnimateLight` runs when the tenth
  of a second or a style changes.

Water's corners shared between polygons do not pay: only two polygons share a corner, and each then
needs a conversion. `RecursiveLightPoint` stays as it is on the core that needs its answer: a shorter
ray is not the same bits.

Checking such a change: a held frame of a timedemo is the same picture each run only with
`+host_framerate 0.05` (without it the flash over the picture fades by real time, and every
pixel can differ by a level or three); `+map` needs `+host_framerate 0.05 +con_notifytime 0
+scr_conspeed 100000` and `QUAKE_SERVER=inline`. Frame 150 of demo1 is a fight, frame 420
has models, text and the status bar. `+forward` typed after "track 30:" lands in whichever
server frame is running: two builds of different speed walk the same way a frame or two
apart and stop at the same place.

## No doubles in a frame

The machine has float instructions and no doubles: a double addition is a call of some 100
instructions, a division of over 200. So nothing a frame does is in doubles:

- **The clock is whole milliseconds.** `realtime_ms` is an unsigned count (`Sys_Milliseconds`, the
  machine's clock word), and the main loop hands `Host_Frame` the milliseconds that passed. A stamp
  is a count, a time since one a float (`REALTIME_SINCE`), a thing that only turns takes the count's
  remainder (`REALTIME_TURN`: the cursor's blink, the sky of `-nokeep`). A float of seconds would be
  too coarse after some hours; the count wraps after 49 days and differences are right across that.
- **An entity's time of its last message, the server's time of its last look for a client and the
  view's time of its last stop** are floats, compared with the game's time, which is a float.
- **A variable's value** asked for by name (QuakeC's `cvar`) is the value kept beside the text.
- **The projection**: OpenGL's doubles are floats (`include/GL/gl.h`); the tangent of the view's angle
  is worked out in doubles once for an angle, so the picture's numbers are the same to the bit.
- **The network keeps its doubles** (`net_time`, a connection's stamps), and so do the timers nobody
  leaves on (`host_speeds`). `Sys_FloatTime` works its double out only when the clock has moved, and
  the network's poll asks only when something is scheduled.

To find such a routine's callers: `objdump -d` of the game's ELF, every `jal` to a soft-float routine by
the function it is in, beside that function's share of a profile. Without `+host_framerate`, a frame's
length is the milliseconds times 0.001 in a float.

## A pass's instructions

A pass of the machine is up to 8,192 instructions, and what ends one early loses the rest
(`docs/machine/boot.md`, "Passes that end early"; `tools\tick_util.ps1` measures it):

- **The swap is not waited for** (`seglSwapEarly`, `docs/machine/gpu.md`): the list is submitted and the
  next frame's sound, input, server and demo run while it is drawn. What the list may still read is
  settled for where it is written: a texture, a pose or a light table going into the pool (`resident`,
  `qglModel`, `qglLights`, the texture uploads, `qglLight`, `qglBlock`) and the palette. Everything else
  a frame writes is its own set.
- **A particle is three texels of 16 bytes**, and the array is aligned to them: its place and colour,
  its speed and ramp, the rest. A frame stores to the first, or the first two.
- What is left of early ends is a level's start: its textures copied into the pool, 6 KB a pass, which
  is all of new stores a pass keeps (`memcpy` from `resident`).

    QUAKE_SWAP=wait       every list waited for
    QUAKE_HOLD_LATE=1     with QUAKE_HOLD=N: frame N stays while the game makes frame N + 1
                          ready, and stops before it is shown

To check a change to what is written when: `QUAKE_HOLD=420 QUAKE_HOLD_LATE=1 quake +timedemo
demo1` with `--gpu-capture`, once with `QUAKE_SWAP=wait` and once without: the two pictures
of frame 420 must be the same pixel for pixel. A plain hold cannot show a store that came
too early, since the game stops before it makes one.

## The server on a worker core

A frame of a single-player game is the server's (physics, the monsters' QuakeC) and then the client's
(the picture). On a machine with worker cores (`docs/machine/multicore.md`) the server's physics runs
on one, as a function of the game in the game's own memory (`linux/quake/server_shaderemu.c`), and the
two go at their own pace:

- **The client does not wait.** When the server's frame is still out, the client's frame goes
  on without one: it draws from the server's last two answers, moving things between them,
  which is what Quake's client does in a network game (the game only switched that off for a
  server in the same program). When the server's frame has ended its messages are sent, and
  the next is begun with the moves the client made meanwhile: at once, from a few places in
  the client's frame (`SE_ServerPoll`) and from the client's refused frames (`SE_ServerIdle`).
- **The frame's messages are made on the worker too** (what the client is told: the entities
  it can see). They are kept there, and the first core puts them into the connection when it
  takes the frame, because the client reads its end meanwhile.
- **The server's frame has its own length** (`sv_frametime`: the time since its last one began,
  0.1 s of the game at most, as a frame of the game always was). Where that limit holds, the
  game's time goes slower than the clock, and the client's time goes at that rate between the
  server's frames: at the slower of the last two frames' rates and a tenth slower still, so that no
  frame stands still (the quakestat: line counts them); the client is a little behind when the next
  frame comes instead, and is taken up to it.
- **The server keeps to its own data.** Console lines, commands and cvar changes that QuakeC
  makes on the worker are kept and done by the first core afterwards; an error there ends
  the job and is raised on the first core; the server's random numbers are its own (the C
  library's have one state, which the client uses too). A console command that may be the
  server's (anything but the keys' and the like) waits for the server's frame to end.
  Loading, leaving and demos are on the first core.
- **Every variable has its own 16 bytes** (`-fno-common -fdata-sections`, and the sections
  aligned by `objcopy` in `build.sh`): two cores must not store to the same 16 bytes in one
  pass of the machine.
- The client reads what it was sent before the server is replaced (`SE_ServerDrop`), or the
  "reconnect" of a level change cannot be sent. A button counts as down for a frame of the server's
  if any of the client's moves since the last had it down, or a tap between two of its frames (a
  jump, a shot) is lost.

Above ten server frames a second the game's time is the clock's, and a frame is shorter than a
monster's tenth of a second, so fewer think in it. The server's frame's end is a word: its frame
writes its number, and `SE_ServerPoll` asks the library only when the word says so, or once a frame (a
page the worker stopped at is served then).

    QUAKE_SERVER=inline   the server on the first core
    QUAKE_SERVER=late     on the first core, its messages sent at the end of the client's frame
    QUAKE_SERVER=wait     on a worker, the client's frame waiting for it at its end
    QUAKE_SUM=N           a sum over every entity's fields each N frames of the server
                          (but the world's model and the players' names, whose places depend on the heap)
    QUAKE_NET=1           what a packet costs, every 200 sent (the game has its UDP driver: docs/system/lan.md)
    QUAKE_TRACK=N         where the player is and how fast, each N frames of the server
    QUAKE_TEST_EXIT=F     (with QUAKE_TRACK) at frame F the player is put into the level's exit
    QUAKE_TEST_PLACE=F,x,y,z,pitch,yaw   (with QUAKE_TRACK) from frame F the player stands there, still

`late` and `wait` do the same things in the same order, one on a core and one on two. With
`+host_framerate 0.05` (a frame's length is then not the machine's speed) and `--fixed-dt
0.004`, `QUAKE_SUM=10` must print the same sums with `inline`, `late` and `wait`: that is the
check that the server on a worker computes what it computed on the first core. The default
cannot be checked so (how many frames the client draws to one of the server's is the
machine's speed); it runs the same job.

## The other workers, and draws made again

The program asks for thirteen workers (`SE_Workers` in `server_shaderemu.c`; `QUAKE_WORKERS=N`, 1 is
the server's only, 0 none). Each part is the first core's with fewer.

- **Light maps that flicker are the workers'** (`light_share` in `world_shaderemu.c`). When a style's
  brightness changes, ten times a second, workers make again every light map of the level that has
  that style, into the light map's own texels in GPU memory, in one call. A page of light maps has one
  worker (two surfaces side by side share 16 bytes). A brush model's are the workers' too.
- **Water is kept in GPU memory as the level is**, a run a texture, and its ripple is written there by
  workers: the texture coordinates of the water the viewer's leaf can see, as often as they get round
  (`water_share`; one worker to 160 quads).
- **With both, and the sky kept, a frame asks nothing of a leaf**: no surface goes on a chain, and the
  torches and the like that stand in the leaves in sight are a list made when the viewer changes leaf,
  each tested against the view by its own box (`static_add`).
- **Brush models are kept too** (`brush_build`): a door's or a box's triangles lie in GPU memory, sorted
  by texture and again by light map, and a frame is its matrix and a draw a run. All of its faces are
  drawn, not those that face the viewer: the others are behind them.
- **A draw made again is its command copied** (`gpu_shaderemu.c`, which is `gles.c` and a few calls
  after it; `qglModel`, `qglTrianglesAt`). A model's pose drawn with the skin and the pose where they
  were in GPU memory is the sixteen words it was drawn with before, with the frame's matrices and the
  light's table. An entity's matrix is written as rows where the draws read it (`qgpuRows`), and nothing
  is pushed or popped: the next entity's replaces it, and the world's is put back when something else is
  drawn (`qglWorldAgain`).
- **The status bar is the frame's first draws when it can be** (`SE_SbarHead` in `gl_screen.c`,
  `qglHead`): the bar of a game being played, the same for three frames, with no tint over the picture.
  A frame is built in one of two sets of memory in turn; when this set's last frame began with the same
  bar, the commands, vertices and matrices are still there and are counted, not written.
- **Particles are workers'** (`se_particle_share` in `r_part.c`): each share has a quarter of the
  array, moves its particles and writes a vertex each into a buffer of its own; the first core gives a
  new particle a free place.

    QUAKE_LIGHT=frame     light maps made by this core, in view (and brush models a polygon at a time)
    QUAKE_WATER=frame     water drawn into the frame by this core
    QUAKE_PARTICLES=frame particles moved and drawn by this core
    QUAKE_LIGHT=wait, QUAKE_WATER=wait, QUAKE_PARTICLES=wait   the workers, waited for in the frame
    QUAKE_MARK=leaves     the loop over the leaves in sight
    QUAKE_BRUSH=frame     brush models a polygon at a time
    QUAKE_SBAR=frame      the status bar drawn where it was, every frame

With `wait` a frame's picture is what the first core's own work made of it; without, a light
map is some passes behind its style, the water's ripple a frame behind the clock and a
particle a frame behind its place.

Not attempted: the status bar in pieces, for a fight in which it changes every few frames; a demo's
messages read ahead.

## Calls of the second layer, and the frame's jobs

Every job of the program is a call of the workers' second layer (`programs/mc/mcwx.h`,
`docs/machine/multicore.md`; `build.sh` compiles `mcwx.c` beside `mcw.c`): `mcw_call` gives it to a worker
that has nothing to do, `mcw_ready` asks after it and `mcw_join` takes it. No worker is the server's or the
particles': the server's frame, a loader's job, the light maps and the frame's jobs below each go to
whichever is idle (`SE_GroupAdd` and the like in `server_shaderemu.c`). A call costs this core several
hundred instructions from `mcw_call` to `mcw_join` (a worker is looked at when it is chosen, when it is
asked after and when it is joined), so the pieces of a frame are as few calls as they can be:

- **The frame's jobs** (`SE_FrameJobs` in `world_shaderemu.c`, after `CL_RelinkEntities`) are one
  call: the light under the models that moved, the water's ripple and the particles. A fight's
  particles (more than 512 a call) and much water take more calls. Water is rippled for the
  leaves the viewer saw the frame before; particles a call moved before the frame draws them are
  drawn in that frame.
- **The light under a model that moved** is walked down the level by the worker
  (`SE_LightAheadPrepare`, `SE_LightAheadJob` in `gl_rlight.c`) while this core goes on to the
  picture. The walk is all that is kept; the sample's light is worked out again when the model is
  drawn, by the styles as they are then, so the light is the same bits. A model out of view the
  frame before is left to this core, and an answer not there yet in time is worked out here (the
  worker says it has made them by a word, a pass later). The weapon is placed later (`V_CalcRefdef`)
  and drawn after the rest of the view: its walk is a call of its own (`SE_GunAhead`). The cache of
  points already walked has 256 places. `QUAKE_LIGHTPOINT=check` makes every walk a worker made here
  too and counts those not the same (the `quakestat:` line about the light): it must say 0.
- **An explosion's 1,024 particles are made by their shares' workers** (`se_explode` in `r_part.c`, and
  `R_RunParticleEffect` of 1,024). This core gives each a free place and writes its number in the
  explosion beside it; the worker makes it from the explosion's place and the seed `rand()` had,
  stepped on by seven numbers a particle at once (a step taken 2^b times is a multiply and an add:
  `se_leap`), and this core's `rand()` goes on as if the 7,168 numbers had been drawn. When each share
  has a quarter free, the places are written a share at a time. A particle is seen a frame after it is
  made.
- **The particles' vertices are in GPU memory** (`qglParticleMemory`, 128 KB taken from the pool;
  `qgpuPointsAt` in `gpu_shaderemu.c`), and each share is a draw from where its worker wrote them.
- A worker without a call sleeps: they have calls only, never a team that lingers.

    QUAKE_EXPLODE=frame        an explosion's particles made by this core
    QUAKE_PARTICLES=copy       the workers' particles copied into the frame
    QUAKE_PARTICLE_SUM=N       a sum of the particles drawn every N frames, and rand()'s seed
    QUAKE_LIGHTPOINT=frame     every walk for a model's light made by this core
    QUAKE_LIGHTPOINT=check     and made here too when a worker made it, and compared

## The picture on a worker core

A frame's drawing is a worker's, a frame late (`scene_shaderemu.c`, `SE_SceneFrame` in place of
`SCR_UpdateScreen` in `_Host_Frame`): everything that writes the GPU's list and swaps it, the level, the
models, the particles, the 2D screen (`SE_ScreenWorker` in `gl_screen.c`). The first core gives frame N out
and goes on to frame N + 1: the input, the server's part, the demo's or the server's messages, the entities
placed.

- **What the worker reads that the first core goes on changing, it reads from a copy** made when
  the frame is given out: the client's state (`cl`, but its lists of models and sounds, which only
  a new level changes), the entities drawn (from their place on: not the messages' and baselines'
  fields, about 100 bytes of each), the dynamic lights, the frame's count, length and time, and the
  view's blend. The files that draw see those through `include/se_scene.h`: `cl`, `cl_entities`,
  `cl_visedicts`, `cl_dlights`, `host_framecount`, `host_frametime`, `realtime_ms` and `v_blend` are
  pointers there, at the copy while a worker draws and at the client's own otherwise.
- **What only the first core changes, it changes between the two frames**, when the worker has
  none: the view (`V_CalcRefdef`, `SE_ViewFront` in `view.c`), what `R_SetupFrame` made of the
  client's state (the light styles, the view's leaf, its contents' colour and blend:
  `SE_SceneFront` in `gl_rmain.c`, the same sums in the same order), the light maps' call
  (`SE_LightFrame`), the frame's jobs (water, particles, the light under models that moved and the
  weapon, whose place is known then), and the palette's decay (`V_UpdatePalette`). The light maps'
  call and the palette are made after the frame is given out: the frame does not read them, and
  the time between two frames is the frame's.
- **Anything else of the first core's that changes what the frame draws waits for it first**
  (`SE_SceneOwn`): a line printed or centred, a key, a command, a setting, a skin made for a player,
  a window resized, a level loaded, cleared or left, an error, and every message of the server's but
  a frame's everyday ones (`cl_parse.c`). A line the worker prints is written when it is joined
  (`mcw_print`); an error there ends the program (`mcw_fail`), and so does a setting or a command
  made there. The server's own lines, settings, commands and errors go where they go on the server's
  worker: `SE_ServerOnWorker` is true there only.
- **A frame is drawn here** in a menu, the console, a level's first frame, an intermission, a pause, a
  holodeck's seat, with mirrors or `lcd_x`, a held frame drawn late, a setting of the view's size
  changed, and with `QUAKE_SCENE=inline`, without workers, or with the particles on this core
  (`QUAKE_PARTICLES=frame`, or fewer than thirteen workers).
- The first core waits for the frame by the word its call writes last, and asks the library only every
  fourth pass: `mcw_join`'s wait looks at every worker each pass.
- **The worker hands its list to the first core** (`qglSwapHand`, `seglSwapHand` in `gles.c`), which
  submits it where it looks anyway: the main loop, `VID_Phase`, `count_swaps`, the waits (`seglSwapTake`).
  The worker never takes the GPU's lock word, which the window system on the first core takes too: there
  are no atomic instructions between cores.

`quakestat:` says `N of these frames drawn by a worker core, K thousand instructions each, W waited for`.

## Drawn without a gap, the 2D screen on a worker of its own

The drawing is the frame:

- **The drawing worker stays** (`scene_job`): one call that draws frame after frame, each from one
  of two copies in turn, and goes on to the next as soon as it has swapped one if the first core
  has given it. The first core fills a copy while the worker draws from the other, waiting only
  until the frame two before has been swapped (that copy's last reader), and says "no more"
  before anything that changes what drawing reads (`SE_SceneOwn`). A frame is so shown up to two
  frames of the first core's after it was made. Between frames a worker with nothing to draw ends
  its passes (`mc_end_pass`).
- **The view is in the copy too**: the refdef, the view's place and axes, its leaf and the light
  styles' values (`se_scene.h`), with which the first core makes the next frame's view while the
  worker draws the last. Code of the first core's in the files that draw reads its own state again
  between `se_real.h` and `se_scene.h` (`SE_SceneFront`, `R_AnimateLight`, the walks, the frame's
  jobs, the light maps). The particles' vertices are in four buffers: one being written, one
  shown, and two that a frame given out or a list being drawn may still read; a job writes into
  one of none of those, and no frame waits for the GPU for it. The walks' tables are two, a copy's
  each, and one is taken for walks only when none is out.
- **A worker waiting for the GPU ends its pass** (`rest` in `programs/linux/gles.c`): its pause would be
  a sleep until its job changed, which a call that draws frame after frame never has. The sky's place
  on the picture, set every frame, is written in the set being built only (`seglKeptLaidHere`).
- **The 2D screen is recorded by a third worker** (`record_screen`, `SE_ScreenRecord` in
  `gl_screen.c`) from the same copy, while the drawing worker draws the view: the status bar, the
  notify lines, the centre print, the crosshair, the icons, recorded as rectangles into GPU memory
  (one of three buffers) and runs of one texture and tint (`qglRecordBegin` .. `qglRecordEnd` in
  `qgl.c`: on the core that records, the texture, the colour and the rectangles go into the
  recording, anything else spoils it). The drawing worker draws the runs after the view, a command
  each pointing at the rectangles where they lie (`qglRecordDraw`, `seglSpritesAt` in `gles.c`),
  waiting for the recording if it is not done. `gl_screen.c` and `sbar.c` read the client's state
  through `se_scene2d.h`, the copy of the frame being recorded. A frame with the console down, the
  scores, a menu or a smaller view is drawn by the first core; `quakestat:` counts frames whose screen
  could not be recorded.

    QUAKE_SCENE=inline     every frame drawn by the first core

## The server's frame

A busy level's server is most of the game's work: its monsters' moves and looks are traces through
the level. What it is made of, and what is done about it:

- **QuakeC's statements are decoded once** (`PR_ExecuteProgram (-1)` from `PR_LoadProgs`): the code of
  each op and its operands as byte offsets, a `goto` to the op's code, the same operations on the same
  values. In `malloc`'s memory, not the hunk's: anything moved there moves QuakeC's strings, and the
  sums with them.
- **A hull's clip nodes and planes as one** (`se_node_t` in `gl_model.h`, `SE_NodeContents`,
  `SE_NodeCheck` in `world.c`): a node is one 32-byte record, its child chosen without a branch. The box
  hull, made again for every entity, walks as before.
- **A variable QuakeC asks for by name** is found by where the name lies first (`PF_cvar`).
- **A move through a BSP model's hull is kept** (`se_traced` in `world.c`, 512 places; `QUAKE_KEEP=0` keeps
  none): the same start, end and offset in the same hull are the same answer while the level lasts. It
  serves mostly the ideal pitch's traces of a player standing still. Kept points and the leafs an
  edict's box touches find too little to pay.
- Smaller: `SV_Move`'s memset of a clip whose every field is set, a pusher's loop over every entity
  testing the box first, `PR_EnterFunction` and `PR_LeaveFunction` copying through pointers of their own
  (every store could have been `f`'s to the compiler).
- `Mod_DecompressVis`'s buffer is one for the server's worker and one for the client's cores
  (`SE_ServerOnWorker`), on 16 bytes.

## The server's frame made shorter

Measure on the game played, never the timedemo: `python tools/game_par.py quake` (e1m5 from where it
starts, the server on its worker). The `quakestat:` line "the server's frame in ms of the clock" says how
long it took to begin, ran, waited until this core took it and until the next began. A frame of the server's
that gets shorter has fewer monsters thinking in it, so compare the same work too: `QUAKE_SERVER=wait
+host_framerate 0.1`.

What could still be taken, none of it tried: the ideal pitch's traces, the water check of a monster that did
not move and the floor check's point tests (whether each can be kept from one frame to the next with the same
sums is not looked at). Workers could answer into a table kept by the question (start, end, box) the walks of
the clip tree and the point tests a monster will probably ask, as Tiberian Dawn's path searches do
(`docs/games/tdawn.md`): an answer found is the same bits, one not found is worked out as now. Measure first
how many steps a guess from the frame's beginning would hit. QuakeC as machine code is the most work for the
least certain gain. Thinks side by side on a copy of the frame's beginning would give other results (random
numbers a monster, two monsters wanting one place): the sums would not be the one core's.

The first core writes every page of Quake's GPU memory once at the start (`qglOpen`, `amoor` of nothing):
memory a worker will store to wants a store by the first core first. The `quakestat:` line of the drawing
worker says how many pages were brought for workers, and how long a frame was drawn, swapped and waited for.

## Checking it

`tools/test_all.py`'s `quake-*` cases, under `rvc_cpu`:

- `quake-server-inline`, `-late`, `-wait`: `QUAKE_SUM=10 quake +map e1m5 +host_framerate 0.05` with
  `--fixed-dt 0.004` must give one digest (`52463017`) in all three modes.
- `quake-particles`: `QUAKE_PARTICLES=wait QUAKE_PARTICLE_SUM=1 quake +timedemo demo1 +host_framerate 0.05`, one
  sum at each frame (digest `9c67f307`). `QUAKE_PARTICLES=wait` waits for every one of the frame's jobs, the
  walks too.
- `quake-light`: `QUAKE_LIGHTPOINT=check quake +timedemo demo1` says `0 not as made here`.
- `quake-walk`: `QUAKE_TRACK=5 quake +map e1m1 +host_framerate 0.05` with `+forward` after "track 30:" stops at
  480 687 24.
- `quake-exit`: `QUAKE_TRACK=5 QUAKE_TEST_EXIT=40 quake +map e1m1` with `+attack` after "track 80:" ends the
  level, loads `maps/e1m2.bsp` and goes on.
- `--races`: `RVC_RACES` on demo1 and on e1m5, 0 undone. Reads as they were are the words a core waits on: the
  drawing worker's look at what it is given (`published`, `stop`), the 2D worker's word, the walks' word, every
  `qgl` call's look at whether it records (`rec_on`).

By hand:

- The GPU against its model: hold a frame, pass `--save-state` and `--gpu-capture`, and run
  `python tools\gpu_reference.py SNAP BMP --size 640x480` (the picture is the window's size, not the
  display's). The pixels away from triangle edges must agree.
- A change to how it draws: hold the same frame before and after and compare the window's pixels, by
  `rvc_harness --cpu --warp`: frames 150, 300 and 420 of demo1 held (`QUAKE_HOLD`, `+host_framerate 0.05`,
  `+con_notifytime 0`: the notify lines go by the clock), with `QUAKE_SCENE=inline`, with the jobs waited for and
  at their own pace. A flickering light is up to a tenth of a second behind without `wait`.
  `QUAKE_WATER=wait` against `QUAKE_WATER=frame` shows the water.
- To see where instructions go, snapshot a run at a `quakestat:` line and resume it with
  `--pc-log` (`tools\pc_profile.py`, `quake=quake.nm`), or `python tools/prof.py` (`docs/machine/profiling.md`).

## Not done

- Music, and hearing the sound by ear: it was checked against the card's model only.
- Playing it: keys and the pointer by hand, in the harness's window and in the VRChat world.
- The server's questions on workers and QuakeC as machine code ("The server's frame made shorter").
- Loading: the light maps' placement is a search of every block for every surface.
