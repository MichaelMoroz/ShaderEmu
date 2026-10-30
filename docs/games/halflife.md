# Half-Life: Uplink

Xash3D FWGS with the HLSDK, built for Linux without an MMU (`linux/halflife/build.sh`, one of
`linux/nommu/build.sh`'s scripts), playing Valve's Uplink demo. The engine is in the shared image
and in `linux/prebuilt`; Uplink's data is not ours to pass on, so it is never committed: when an
image is made (`linux/nommu/image.py`, through `tools/make_image.ps1`) `linux/halflife/data.py`
fetches the installer from archive.org (md5-checked), unpacks it with REWise (`data.sh`, in WSL),
repacks it and puts it into that image's overlay. `HALFLIFE_DATA=0` makes an image without it,
and the starter then says the data is missing. The download and unpacking happen once
(`build/halflife`); a made image takes 2.5 s more with the repack cached.

    wsl -- bash linux/nommu/build.sh halflife/build.sh
    wsl -- bash linux/nommu/root.sh   (the built root made flat: without it the image keeps the old program)
    pwsh tools/make_image.ps1
    halflife                       (in the image; the desktop's and the start menu's Half-Life: Uplink)
    halflife -dev 1 +map hldemo1   (straight into the level, its messages on the console)

The icon is an orange lambda in a ring of our own drawing (`tools/draw_icons.py`), not Valve's logo.

The engine's changes are `linux/halflife/xash3d.patch`, `hlsdk.patch` and `mainui.patch` (the
menus); the machine's own parts are beside them: OpenGL 1.1 on the GPU (`gl_shaderemu.c`), the
level kept in GPU memory (`gl_kept.c`), studio models on workers (`gl_studio_job.c`), the server's
frame on a worker (`sv_shaderemu.c`), the window (`vid_shaderemu.c`), the films
(`avi_shaderemu.c`), the sound card (`snd_shaderemu.c`) and libmedia's pictures and sounds
(`img_media.c`, `snd_media.c`).

## The data

Uplink's `pak0.PAK` is 79.2 MB; `repack.py` makes it 34.6 MB when the image is made (cached in
`build/halflife/repack-cache`), and the game's folder is 39.7 MB in all:

| | Before | After | How |
|---|---|---|---|
| sounds | 29.8 MB | 18.8 MB | the card's IMA ADPCM with checkpoints, in each WAV (format 0x11, a `ckpt` chunk) |
| maps | 26.5 MB | 5.8 MB | textures once in `uplink.wad` (3.75 MB) without mips; the rest deflate in segments |
| models | 7.8 MB | 5.8 MB | deflate in segments, files of 32 KB or more |
| sprites | 6.7 MB | 2.7 MB | the same |
| pictures | 7.9 MB | 1.4 MB | 24-bit ones JPEG at quality 70 under their own names; the sky's BMP copies left out |

- **Sounds** play where they lie in the ROM, as before: no decoding and no RAM. A voice
  started mid-sound starts at a checkpoint, a multiple of 32 samples. MP3 was tried
  (`REPACK_SOUNDS=mp3`, 8.7 MB, decoded by libmedia when a level registers its sounds): the
  272 sounds of hldemo1, 368 s of audio, took 11.4 s on 42 workers and 22 s with their
  setup, and 8 MB of RAM, after which a 2 MB allocation failed. FLAC decodes only some 10%
  cheaper. The decoder stays in `snd_shaderemu.c` for that switch.
- **Deflate in segments** (`docs/system/media.md`): the filesystem inflates such a file when it
  is opened (`FS_OpenInflated` in `filesystem/pak.c`), its pieces on the workers, and serves
  reads from memory. Smaller files stay as they are: an 8 KB piece is about 40 ms on the card
  and a small file waits for one whole (176 files took 7 s; 64 files of 32 KB or more, 3.6 s).
- **Textures** of a name with two pictures in two maps stay in their map. A picture with
  `offsets[1] == 0` is one level with its palette after it (`Image_LoadMIP`). WAD textures are
  kept as 8-bit, as a map's own are, so the GPU draws them paletted.
- A JPEG is told by its first bytes whatever its name (`Image_ProbeLoadBuffer_`).

Smaller data does not make a load faster here: the ROM is copied, not read through a disk,
and every byte saved is decoding work. Launch to "client connected" on the card: 61 to 66 s
before, 65 s after.

## GPU memory

The pool is 28 MB of RAM (`HL_POOL_MB`), which the GPU reads as it reads its own memory: on
the machine without an MMU a pointer is its address, the GPU memory's mapping too.
It was the GPU memory's 7.27 MB left over, and with the desktop open the level was drawn unlit
in the world. `HL_POOL_MB=0` is that pool again. The menu's pictures (`pfnPIC_Load`) and the
static light maps are marked `hlglSpillable`: when the pool has no room, those not drawn for two
frames move out and come back when drawn. Blocks of 64 KB and more are taken from the top of the
pool. Greyscale and 4-bit pictures are paletted too.

## The start

As the demo's launcher did: its splash (`logo.bmp`, drawn into the window as soon as the engine
starts, `SE_Splash` in `vid_shaderemu.c`; `-nosplash` leaves it out), the opening film
(`media/intro.avi`, the "Game of the Year" montage, 54 s, which Escape or a click skips; `-nointro` leaves it out), then the main menu
with the animated Uplink logo over it (`media/uplink.avi` as `logo.avi`). The menu is mainui's
with `linux/halflife/mainui.patch`: for a game of one player (`type "singleplayer_only"` in
`gameinfo.txt`, which the engine itself overrides for a folder called `valve`) it has the demo's
buttons, New game, Configuration, Load game, View Readme.txt, Order Half-Life and Quit, without
Multiplayer, Custom game or Previews; View Readme.txt shows the demo's `readme.txt` in a list
(`menus/Readme.cpp`), and Order Half-Life opens the demo's own order page with `open`
(`Platform_ShellExecute` in `sys_posix.c`). New game asks Easy, Medium or Difficult and starts
hldemo1 at that skill. The menu's sounds are the launcher's (`media/launch_*.wav`, in pak0). No
`liblist.gam` is copied: newer than `gameinfo.txt`, the engine would take Multiplayer and the
first map from it. The engine reaches its first frame 2.5 s after it starts (`rvc_harness --cpu`).

The films are played by `avi_shaderemu.c`, which replaces FFmpeg's: Microsoft RLE and plain
8-bit frames, a picture a frame into one texture. A film's sound is played by the card where it
lies in the ROM when it is one chunk: `data.py` moves the opening film's sound in front of its
frames.

## Speed

`HL_MARK=FROM,TO` prints `hlmark: begins` and `hlmark: ends, N frames` at those seconds of the
level, for `rvc_cpu --count-from` and `--until`. On 18 October 2026, hldemo1 on `rvc_cpu` (core 0
a frame, frames a second at 428 passes a second):

| | core 0 | fps |
|---|---|---|
| the main menu (`FILM_LOOPS=1`, the logo's loops 4 to 8) | 185k | 19.0 |
| the start, 3 to 25 s | 182k | 18.2 |
| the fight at 31 to 45 s (19 studio models, 51 solid entities) | 407k | 8.1 |

The menu was 2,070k a frame (1.7). The logo film (640 x 100, 110 frames, mostly deltas) is
decoded whole once on a worker and kept (`film_give` in `avi_shaderemu.c`; a film of 8 MB
decoded at most and no sound; `FILM=read` decodes as it plays); its first frame is held until
then, about 6 s on the card (decoded on core 0 as well, the menu's first 8 s were 1.9 fps and
New game in them never loaded the level, "A job posted to a worker still at another" in
`docs/machine/multicore.md`), and a frame's tiles are copied
into its texture on a worker (`hlglTexLater`). The menu's text is drawn again from its quads of
the last time (`UI_DrawString` in `mainui.patch`, `CL_DrawString` in `xash3d.patch`, through
`hlglCaptureTo` and `hlglDrawQuads`): the first character is drawn and compared, the last drawn
to leave the engine's state as it was; `HL_MENU_TEXT=now` and `HL_TEXT=now` draw it as before.
`HLGL_PICSUM=1` prints a sum of the frame's 2D quads every 64 frames: the menu's must be the
same both ways (`8ffbd714`). While a level's drawing is on its worker (`SE_RenderPiped`) a text is drawn as it is:
its quads are recorded, not made, so there was nothing to keep, and before 21 October 2026 a
`say` line lost its name's first letters ("root:" drawn "t:").

A studio model's bones are set up ahead on four workers when the entities are emitted
(`R_BonesAheadGive` in `gl_kept.c`), and the draw takes its rotation from there too
(`SE_StudioRotationTaken`; `HL_ROTATION=inline` makes it at the draw, `HL_ROTATION=check`
makes both and prints how many differ: it must say `0 of them wrong`). What is left of the fight
on core 0 is many pieces: the studio models' draws 93k (their setup and hand-off to the jobs
45k, light 12k, culling 8k), the player's prediction 48k, the entities' parse 40k and their
emission 42k. A frame of 15 there needs the studio draws off core 0 whole, with their GL state.

### The drawing a frame behind

Since 21 October 2026 the game draws frame n's 3D on a worker core of its own (`rnd_shaderemu.c`,
worker `workers - 8`) from copies of its entities (`R_SceneGive` in `gl_kept.c`) while core 0
makes frame n + 1, then frame n's 2D. `HL_RENDER=wait` puts the drawing on
the worker with core 0 waiting, `HL_RENDER=core` draws on core 0 (the default before). Every
renderer function core 0 calls goes through a thunk (`se_ref_guard`, its kind of call in
`ref_kind`): the scene's additions go to core 0's own list, and the frame's start and its 2D
(`R_BeginFrame`, `R_Set2DMode`, `R_DrawStretchPic`, `FillRGBA`, `Color4ub` and a few more, all
void with values for arguments) are recorded while the drawing is out, in one of two lists, and
the frame's own drawing replays them in order after its 3D (`se_ref_replay`: a0 to a7 and the
caller's first four stack words, floats too under ilp32): core 0 only waits for it at the swap
(`R_EndFrame`) and gives it the list with the next frame. A render mode or colour the same as the
last recorded is not recorded again. Any other call settles first, and core 0 replays what it had
recorded then; a call from a worker (core 0's own while the drawing is out) goes through.
`HL_2D=core` replays the 2D on core 0 at the swap, over the frame before's 3D (before 21 October
2026: frame n was shown with frame n + 1's 2D). `HL_LATER=0` draws the 2D after a settle instead. `HL_RENDER_CALLS=1` prints
what core 0 calls of the renderer between frames, what made it settle, and the job's timeline
every 32 frames.

What the drawing's worker writes that core 0 also writes is its own: the light's trace state
(`trace_of[2]`), its line-of-sight traces (`CL_VisTraceLine` with a playermove of its own,
`se_render_pm`), the local light level (kept in `se_render_light_level` until the settle), and it
neither counts `r_stats` nor copies a live entity's attachments. `RVC_RACES` says 0 undone in
the fight with it and with `HL_RENDER=core`. Its traces still read the client's physents as they
are while core 0 makes the next frame's ("as it was" reads, a frame's lag at most).

The studio models' points go to ring jobs on the other workers (`ring_post` in
`gl_shaderemu.c`); the job takes a ring that is free before waiting on one, and its ring jobs
are waited for at the swap by core 0, not at the job's end (`jobs_wait`; the give waits for the
late ones, which may read the copies). A job's other parts copy it on their own worker.

The server's frame is on a worker (`sv_shaderemu.c`) and paced: core 0 gives it a frame only
when the last is done, with the time of the frames skipped, so the server runs below the
client's frame rate when it is the longer (`HL_SERVER=every` gives every frame, `inline` runs
it on core 0). The packet's entities are packed by four parts on three packers and the
server's helper (`SE_PackJob` in `xash3d.patch`); the client and the server step their
entity rings by whole blocks, and the light styles skip the time of styles not used.

A paced server leaves several of the client's commands unanswered, often 3 or 4. Before
20 October 2026 two things lost the walk, measured on hldemo1 as updates whose place the
client had predicted wrong (out of 400, walking in a circle; `HL_SERVER=every` gives 2):

- `XASH_LOW_MEMORY=1` gave the client a ring of 4 updates, and the prediction stops at 3
  unanswered: the view stood still, then jumped to the server's place. It is 16 now
  (`CL_InitEdicts` in `xash3d.patch`; the server's stays 4). 18 of 400 before, 2 after.
- The loopback keeps the last 4 packets: the client's packets of a frame out went to it at
  the settle and the older ones were lost. They go whole to the server's next frame now
  (`SE_ServerTakePacket`; `HL_INBOX=loopback` is the old way). 42 of 400 before, 18 after.

A packed file (a map, a model, a sprite) is not opened by a worker's job: `FS_OpenInflated` refuses
it there and the open fails, and opens it on core 0 for the server's worker and the drawing's (below).
A level change the game makes in the server's frame (a `trigger_changelevel`:
hldemo1's elevator) is therefore a `changelevel2` command for core 0 (`pfnChangeLevel`), which
reads the next map with the server settled. Before 19 October 2026 the worker read the packed
bytes as the map and its frame never ended: the game stood in the elevator.

A studio model's sequence groups (`scientist01.mdl` to `scientist07.mdl`, `barney01.mdl` and the
like) are loaded with the model by core 0 (`SE_StudioGroupsLoad`, from `Mod_ForName`): the engine
and the client's renderer load one where it is first played, the server's worker or the drawing's.
Before 21 October 2026 that froze the game in hldemo1's first corridor, 28 s into a walk: the server's
and the drawing's workers run on stacks of their own, which `mcw_on_worker` does not know, so the
drawing inflated the group itself with `media_inflate` on workers that were in the rings' loops and
waited for them for ever, and core 0 for it at the settle. Now a file such a worker opens is opened by
core 0 (`FS_OpenInflated` asks through `SE_ServerOnCore0`), and `SE_MediaWorkers` gives no workers off
core 0, while the rings are on or while core 0 makes the server's ask: a decode there is core 0's
alone. hldemo1's groups are 0.74 MB (barney 3, scientist 7, zombie 3).

The walk that found it is `rvc_cpu` with the card's model on the desktop (`--desktop`, `RVC_SOUND`,
`RVC_WCACHE`, `RVC_ADFAULT`, `RVC_RACES`, `--ips 3100000 --pass-ms 0.2`) and an `HL_SCRIPT` of
`+forward` with a turn every 4 to 8 s, shots, jumps and use, for 1,200 s of the guest's time: before,
the level's clock stopped at 28 s in every run; after, it reached 279 s (`HL_RENDER=core` 274, `wait`
257), 0 undone. It also found statics both the server's worker and another core wrote, each core's own
now (`SE_Who`): `Info_ValueForKey`'s values, the PVS's `g_visdata` (the server's and the drawing's
`R_FatPVS`), and the random generator's state, which the drawing shared with core 0; and the
renderer's lines to stderr write by themselves (`hlglSay`), as stdio's state was core 0's and the
drawing's at once.

The studio models' hit boxes are hulls of each core's own (`studio_hull`, `studio_of[3]` in
`mod_studio.c`), made by the core that first traces against one (`Mod_HullForStudio`); the box
hulls of `pm_trace.c` are each core's too. Before 21 October 2026 only core 0's were made: the
server's worker traced its bullets against hulls with no planes and every shot went through a
monster (a headcrab after the glock in hldemo2 could not be killed; the crowbar hit through the
hull's other test). Now two glock shots of 8 kill it (`HL_SCRIPT`, `ent_info 530`: health -3).
A job of the light ahead (the glows' `SE_VisTraceFraction`) has a box hull of its own too (`box_of[3]`):
under `HL_RENDER=core` it and core 0's prediction wrote the same planes (88 undone).

#### A level that goes, and the sequence groups after it

On 21 October 2026 the game died in the Unity world when the elevator took the player from hldemo1 to
hldemo2. Three things were wrong, each found on `rvc_cpu` with the card's model (the walk's switches,
`HL_SCRIPT` with `-dev 1`: `ent_fire 1 set origin`, `ent_fire startdometalk_mm use`, the elevator's
`startele1` used):

- A level that goes waits first for the server's frame, the drawing and every job (`SE_LevelSettle` in
  `rnd_shaderemu.c`, from `COM_Frame` whenever the host's state is not a running frame). Before, the
  drawing given the old level's last frame at the loading plaque was still out while core 0 freed the
  world and the studio cache (`Mod_PurgeStudioCache`), and the rings stayed on, so core 0 inflated the
  next map alone: the change took 200 s of the guest's, 122 now.
- Whether a sequence group is loaded is its pointer off core 0 (`Mod_CacheCheck`). The check walked
  the studio cache's pool, whose list and the pool table core 0 may be changing (a pool made moves the
  table): a bones-ahead job found `scientist01.mdl` not loaded at the dome's talk, asked for the file,
  was refused (a packed file on a worker), and the `Host_Error` from the worker trashed the heap
  (a capture with `HL_RENDER=core`; it depends on timing). A group a worker still finds missing
  gives the first sequence's pose and says `a worker wanted ...`; it has not been said since.
- Statics of core 0's and a worker's shared a texel: `serving` (core 0's, whether it makes the server's
  ask) beside the server's count of asks (`se_asks`, its own texel now), so a clear of `serving` could
  be undone and `Cbuf_Execute` then ran commands with the server's frame out; the drawing's `pulled`
  vertices beside `se_settle_passes`; and `HL_ROTATION=check` printed with stdio from the drawing's
  worker (`hlglSay` now).

Five level changes in one run (the elevator, two `changelevel`s each way, `map hldemo2`, 3,000 s of the
guest's) pass with the default and with `HL_RENDER=core`, 0 undone; before the last two fixes one
`HL_RENDER=core` run died in hldemo2 (`Mem_FindPool: not allocated or double freed pool`, 1 undone). Barney's `c3a2_draw` at the elevator
(`domebarn_script_4`, which does not turn him: `m_fMoveTo` 0) was the same drawn by the default as by
everything on core 0 (`HL_RENDER=core HL_BONES=inline HL_LIGHT=inline HL_SERVER=inline`): each frame
of one has a frame of the other within 26 to 46 changed pixels of his 200 x 200, against 75 between
two frames. The level's clock ran at 0.56 of the guest's from the talk to the elevator (the server's
frame given at most 0.25 s, `MAX_FRAMETIME`), so the player there outruns the game's people.

Decals and brush models ask for `glPolygonOffset`, which the GPU has no state for: with
`GL_POLYGON_OFFSET_FILL` on `gl_shaderemu.c` moves each point toward the eye along its own ray,
by (factor + units) times 4e-4 of its distance (a decal's (-1, -2) is 0.12%), so it covers the
same pixels and only its depth changes. Before 21 October 2026 the call did nothing and decals
fought the wall: in a decal's 65 x 50 pixels over 16 frames its dark pixels went 54 to 149
(a change of 21.5 a frame), now 165 to 181 (2.7).

On 18 October 2026 (`rvc_cpu`, the card's model: `RVC_SOUND`, `RVC_WCACHE`, `RVC_ADFAULT`):

| | start, 3 to 25 s | the fight, 31 to 45 s |
|---|---|---|
| the default | 24.35 passes a frame, 17.6 fps | 48.15, 8.9 |
| `HL_RENDER=worker` | 23.58, 18.2 | 32.97, 13.0 |
| `HL_RENDER=worker HL_LATER=0` | 20.41, 21.0 | 36.39, 11.8 |

In the fight the drawing's worker is the bound (25 busy passes of 155k instructions, 22
studio models at 4.8k each), then core 0 from the settle to the give (the scene's copy 18k,
the replay 18k). At the start core 0 is: recording the HUD's text and replaying it costs 30k
more than drawing it.

On 21 October 2026, the same way, with `RVC_RACES` (`python tools/game_par.py halflife`, and
`--guest HL_MARK=3,25` for the start; the fight is its default span):

| | start, 3 to 25 s | the fight, 31 to 45 s |
|---|---|---|
| `HL_RENDER=core`, before this day | 26.99 passes a frame, 15.1 fps (core 0 210k) | 55.43, 7.3 (444k) |
| drawn on a worker, its 2D replayed by core 0 (`HL_2D=core`) | 22.23, 18.3 (182k) | 34.83, 11.7 (279k) |
| the default, the 2D replayed by the drawing's worker | 18.14, 22.4 (148k) | 33.93, 12.0 (267k) |

What is left on core 0 in the fight is the player's prediction (`CL_PredictSent`, 84k: 19k a
move, three moves a frame), the HUD (54k), the entities' emission (45k), the settle and the give,
and the server's messages (21k); at the start the opening titles' text (`CHudMessage`, 26k) and
its recording (some 40 characters a frame, three calls each). The replay was 28k of core 0's frame
there (700 instructions a character). A text character asks the font texture's size once in 256
calls (`CL_DrawCharacter`). The titles' and the HUD's pixels at 12 s are the same as before.

### A move predicted once

The client predicted every command the server had not answered, again each frame, from what the
server last said: 2.5 moves a frame at 24k each. The states lie by the command now (`cl_pmove.c`,
`cl_parse.c`; a state is 2.3 KB, the weapons up to Half-Life's id 15, not 6.5), and a frame runs
only the commands not run yet: the moves before them say the same while nothing came from the server, and
when something did, they say the same while it says what was predicted for the command it answers
(`SE_AsPredicted`: place within 0.05, speed within 1, every state that a server may change by itself
exactly). Otherwise it is every move again. `HL_PREDICT=full` is the old way; `HL_PREDICT=check`
runs every move again beside the states kept and prints how far they are apart (`hlpredict:`): 0 over
0.1 and the most 0.016 units in a walk with turns, jumps and shots, 2 replays in 500 frames (a move that
pushes the server's way, a teleport). 31 October 2026, the fight's 231.8k to 205.5k on core 0, 13.1 to
13.8 fps; walking at 3 to 30 s, 164.6k to 133.4k, 19.6 to 24.1 fps.

### What core 0 did between the drawing's end and the next give

A frame is as long as the drawing's job and what lies between its end and the next job's start, or
as core 0's frame if that is longer: core 0 waiting at the swap does not shorten it. Of those 53 units
of the clock (`HL_RENDER_CALLS=1`'s timeline; a frame then 335) the entities' copies were 26. They are made
now at the frame's end while the drawing still draws, into the other of two sets (`R_SceneAhead`,
`HL_SCENE=give` is at the give), and what its end writes back to the entities (attachments, the
frame it was at, a muzzle flash shown) goes to the new copy too (`R_SceneTakeOne`). A cache of a
copy's address breaks on the two sets: the glows' fractions go by the live entity (`R_SceneLive`).
A paint of the sound is once a frame, or an eighth of a second after the last in a long frame, not
at the HUD's end and each time core 0 looks at the drawing (`S_ExtraUpdate`: the card plays by
itself; 2.5k a paint in doubles). 31 October 2026, the fight: core 0 205.2k to 195.1k, 29.55 to 28.05
passes a frame, 13.8 to 14.6 fps.

## The controls

On a desktop the left button held drags the view round, a quarter of a degree a pixel, as
Quake's and Quake III's; the right button fires and the middle one is the second fire. The
pointer's travel is read from the machine's input words (`Platform_RunEvents`), and the window
system is asked for events only when a button or a key changed and four times a second. Use is
E (the demo's `default.cfg` has Tab: `data.py` moves it). The keys are `default.cfg`'s: the engine
runs it when there is no `config.cfg`, as GoldSrc does (`host.c` in `xash3d.patch`; `config.cfg` is
written under `/tmp` and gone at the next start). Before 20 October 2026 nothing ran it, and only
the engine's own table was bound: the arrows, no W, A, S, D and no use.

## From the seat

Played from the holodeck's seat the game reads the host's words (`docs/world/holodeck.md`,
"Played from the seat"; `SE_SeatMove` and `SE_SeatView` in `vid_shaderemu.c`): the eye is 64
units over the floor, the sticks walk and turn, the right trigger fires, the left is the second
fire, the right grip (E on a desktop) is use, Space jumps; with tracked hands the right hand aims and the view model is held in it; with
a mouse it turns the view, 0.066 degrees a count. The picture drawn round the seat leaves no
surface out (`r_nocull`). `HALFLIFE_SEAT=1` prints how it is seated every 20 frames, and
`rvc_cpu --host 00000112,0,0,0,0,00007f00,0,0` walks a desktop's sitter forward.

With tracked hands the shot leaves the hand along its beam: `SE_SeatMove` sends where the hand
is from the eye in the command's `impact_index` (1) and `impact_position`, the game's
`GetGunPosition` adds it and `GetAutoaimVector` gives the view's way with no autoaim and no
punch, and the client's events start from the same place (`EV_GetGunPosition`, `hlsdk.patch`).
The view model points the aim's way, none of the client's bob, sway or autoaim on it, held a
third of its length from its back with its muzzle (attachment 0 at rest,
`SE_StudioAttachmentRest`) on the beam. Before 21 October 2026 the shot left the eye with
autoaim and the model tilted with the bob: 9 shots of the glock from
`--host 80000033,0,0,0,0,0,064ed44b,0e391555` now leave the hand, 0.18 to 0.37 degrees off its
way (the glock's spread is 1), and `HALFLIFE_SEAT=1` says the muzzle is 0.00 off the beam.

`HL_SCRIPT="T:command|T:command"` runs console commands at a level's second T (`script` in
`vid_shaderemu.c`), `look P Y` sets the view's angles and `turn D` turns D degrees a second:
with `rvc_cpu` it is how a fight is played again, run after run.
