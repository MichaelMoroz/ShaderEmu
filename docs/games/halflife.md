# Half-Life: Uplink

Xash3D FWGS with the HLSDK, built for Linux without an MMU (`linux/halflife/build.sh`, one of
`linux/nommu/build.sh`'s scripts), playing Valve's Uplink demo. The engine is in the shared image
and in `linux/prebuilt`; Uplink's data is not ours to pass on, so it is never committed: when an
image is made (`linux/nommu/image.py`, through `tools/make_image.ps1`) `linux/halflife/data.py`
fetches the installer from archive.org (md5-checked), unpacks it with REWise (`data.sh`, in WSL),
repacks it and puts it into that image's overlay. `HALFLIFE_DATA=0` makes an image without it,
and the starter then says the data is missing. The download and unpacking happen once
(`build/halflife`).

    wsl -- bash linux/nommu/build.sh halflife/build.sh
    wsl -- bash linux/nommu/root.sh   (the built root made flat: without it the image keeps the old program)
    pwsh tools/make_image.ps1
    halflife                       (in the image; the desktop's and the start menu's Half-Life: Uplink)
    halflife -dev 1 +map hldemo1   (straight into the level, its messages on the console)

The icon is an orange lambda in a ring of our own drawing (`tools/draw_icons.py`), not Valve's logo.

The engine's changes are `linux/halflife/xash3d.patch`, `hlsdk.patch` and `mainui.patch` (the
menus); the machine's own parts are beside them: OpenGL 1.1 on the GPU (`gl_shaderemu.c`), the
level kept in GPU memory (`gl_kept.c`), studio models on workers (`gl_studio_job.c`), the server's
frame on a worker (`sv_shaderemu.c`), the 3D drawing on a worker (`rnd_shaderemu.c`), the window
(`vid_shaderemu.c`), the films (`avi_shaderemu.c`), the sound card (`snd_shaderemu.c`) and libmedia's
pictures and sounds (`img_media.c`, `snd_media.c`). It is built `-O3 -fno-tree-vectorize
-fsched-pressure -funroll-loops` with GCC's inlining raised, as Quake and Quake III are.

## The data

`repack.py` makes Uplink's `pak0.PAK` about 35 MB when the image is made (cached in `build/halflife/repack-cache`):

| | How |
|---|---|
| sounds | the card's IMA ADPCM with checkpoints, in each WAV (format 0x11, a `ckpt` chunk) |
| maps | textures once in `uplink.wad` without mips; the rest deflate in segments |
| models, sprites | deflate in segments, files of 32 KB or more |
| pictures | 24-bit ones JPEG at quality 70 under their own names; the sky's BMP copies left out |

- **Sounds** play where they lie in the ROM: no decoding and no RAM. A voice started mid-sound starts
  at a checkpoint, a multiple of 32 samples. `REPACK_SOUNDS=mp3` makes them MP3, decoded by libmedia when a
  level registers its sounds: smaller, but a level's sounds then take many seconds of the workers and RAM a
  later allocation needs. The decoder stays in `snd_shaderemu.c` for that switch.
- **Deflate in segments** (`docs/system/media.md`): the filesystem inflates such a file when it
  is opened (`FS_OpenInflated` in `filesystem/pak.c`), its pieces on the workers, and serves
  reads from memory. Smaller files stay as they are: a small file waits for one whole piece.
- **Textures** of a name with two pictures in two maps stay in their map. A picture with
  `offsets[1] == 0` is one level with its palette after it (`Image_LoadMIP`). WAD textures are
  kept as 8-bit, as a map's own are, so the GPU draws them paletted.
- A JPEG is told by its first bytes whatever its name (`Image_ProbeLoadBuffer_`).

Smaller data does not make a load faster here: the ROM is copied, not read through a disk, and every byte
saved is decoding work.

## GPU memory

The pool is 28 MB of RAM (`HL_POOL_MB`), which the GPU reads as it reads its own memory: on
the machine without an MMU a pointer is its address, the GPU memory's mapping too.
`HL_POOL_MB=0` is the GPU memory's own 7.27 MB left over, in which with the desktop open the level is drawn
unlit in the world. The menu's pictures (`pfnPIC_Load`) and the static light maps are marked `hlglSpillable`:
when the pool has no room, those not drawn for two frames move out and come back when drawn. Blocks of 64 KB
and more are taken from the top of the pool. Greyscale and 4-bit pictures are paletted too.

## The start

As the demo's launcher did: its splash (`logo.bmp`, drawn into the window as soon as the engine
starts, `SE_Splash` in `vid_shaderemu.c`; `-nosplash` leaves it out), the opening film
(`media/intro.avi`, the "Game of the Year" montage, which Escape or a click skips; `-nointro` leaves it out), then the main menu
with the animated Uplink logo over it (`media/uplink.avi` as `logo.avi`). The menu is mainui's
with `linux/halflife/mainui.patch`: for a game of one player (`type "singleplayer_only"` in
`gameinfo.txt`, which the engine itself overrides for a folder called `valve`) it has the demo's
buttons, New game, Configuration, Load game, View Readme.txt, Order Half-Life and Quit, without
Multiplayer, Custom game or Previews; View Readme.txt shows the demo's `readme.txt` in a list
(`menus/Readme.cpp`), and Order Half-Life opens the demo's own order page with `open`
(`Platform_ShellExecute` in `sys_posix.c`). New game asks Easy, Medium or Difficult and starts
hldemo1 at that skill. The menu's sounds are the launcher's (`media/launch_*.wav`, in pak0). No
`liblist.gam` is copied: newer than `gameinfo.txt`, the engine would take Multiplayer and the
first map from it.

The films are played by `avi_shaderemu.c`, which replaces FFmpeg's: Microsoft RLE and plain
8-bit frames, a picture a frame into one texture. A film's sound is played by the card where it
lies in the ROM when it is one chunk: `data.py` moves the opening film's sound in front of its
frames.

The logo film (640 x 100, mostly deltas) is decoded whole once on a worker and kept (`film_give` in
`avi_shaderemu.c`; a film of 8 MB decoded at most and no sound; `FILM=read` decodes as it plays); its first frame
is held until then ("A job posted to a worker still at another" in `docs/machine/multicore.md`), and a frame's tiles
are copied into its texture on a worker (`hlglTexLater`). `FILM_LOOPS` says how often it loops. The menu's text is
drawn again from its quads of the last time (`UI_DrawString` in `mainui.patch`, `CL_DrawString` in `xash3d.patch`,
through `hlglCaptureTo` and `hlglDrawQuads`): the first character is drawn and compared, the last drawn to leave the
engine's state as it was; `HL_MENU_TEXT=now` and `HL_TEXT=now` draw it every time. `HLGL_PICSUM=1` prints a sum of the
frame's 2D quads every 64 frames: the menu's must be the same both ways. While a level's drawing is on its worker
(`SE_RenderPiped`) a text is drawn as it is: its quads are recorded, not made, so there is nothing to keep.

## Speed

`HL_MARK=FROM,TO` prints `hlmark: begins` and `hlmark: ends, N frames` at those seconds of the level, for `rvc_cpu
--count-from` and `--until`. `python tools/game_par.py halflife` is the fight at 31 to 45 s of hldemo1, standing where it
starts (19 studio models, 51 solid entities); `--guest HL_MARK=3,30` is the earlier span.

Calibrated, 31 October 2026 (`game_par.py halflife --calibrated`, with `RVC_RACES`), standing where it starts:

| hldemo1 | Frames a second | Passes a frame | Core 0 a frame |
|---|---|---|---|
| the fight, 31 to 45 s | 24.3 | 26.70 | 177.8k |
| 3 to 30 s | 50.5 | 12.85 | 97.8k |

What bounds the fight is the drawing's job (about 130k, 20 models at 4.4k) and the handover of about 2 passes; core 0
waits at the swap. A text character asks the font texture's size once in 256 calls (`CL_DrawCharacter`).

A studio model's bones are set up ahead on four workers when the entities are emitted (`R_BonesAheadGive` in `gl_kept.c`),
and the draw takes its rotation from there too (`SE_StudioRotationTaken`; `HL_ROTATION=inline` makes it at the draw,
`HL_ROTATION=check` makes both and prints how many differ: it must say `0 of them wrong`). `HL_BONES=inline` and `HL_LIGHT=inline`
keep the bones and the light on core 0.

### The drawing a frame behind

The game draws frame n's 3D on a worker core of its own (`rnd_shaderemu.c`, worker `workers - 8`) from copies of its entities
(`R_SceneGive` in `gl_kept.c`) while core 0 makes frame n + 1, then frame n's 2D. `HL_RENDER=wait` puts the drawing on the worker
with core 0 waiting, `HL_RENDER=core` draws on core 0. Every renderer function core 0 calls has a kind in `ref_kind`; a call of a kind
that goes through needs no thunk without `HL_RENDER_CALLS`, and the others go through one (`se_ref_guard`): the scene's additions go to core
0's own list, and the frame's start and its 2D (`R_BeginFrame`, `R_Set2DMode`, `R_DrawStretchPic`, `FillRGBA`, `Color4ub` and a few more, all
void with values for arguments) are recorded while the drawing is out, in one of two lists, and the frame's own drawing replays them in order
after its 3D (`se_ref_replay`: a0 to a7 and the caller's first four stack words, floats too under ilp32): core 0 only waits for it at the swap
(`R_EndFrame`) and gives it the list with the next frame. A render mode or colour the same as the last recorded is not recorded again. Any other
call settles first, and core 0 replays what it had recorded then; a call from a worker (core 0's own while the drawing is out) goes through.
`HL_2D=core` replays the 2D on core 0 at the swap, over the frame before's 3D. `HL_LATER=0` draws the 2D after a settle instead. `HL_RENDER_CALLS=1`
prints what core 0 calls of the renderer between frames, what made it settle, and the job's timeline every 32 frames.

What the drawing's worker writes that core 0 also writes is its own: the light's trace state (`trace_of[2]`), its line-of-sight traces
(`CL_VisTraceLine` with a playermove of its own, `se_render_pm`), the local light level (kept in `se_render_light_level` until the settle), and it
neither counts `r_stats` nor copies a live entity's attachments. `RVC_RACES` must say 0 undone with it and with `HL_RENDER=core`. Its traces still
read the client's physents as they are while core 0 makes the next frame's ("as it was" reads, a frame's lag at most).

The studio models' points go to ring jobs on the other workers (`ring_post` in `gl_shaderemu.c`); the job takes a ring that is free before waiting on
one, and its ring jobs are waited for at the swap by core 0, not at the job's end (`jobs_wait`; the give waits for the late ones, which may read the
copies). A job's other parts copy it on their own worker.

The server's frame is on a worker (`sv_shaderemu.c`) and paced: core 0 gives it a frame only when the last is done, with the time of the frames skipped, so
the server runs below the client's frame rate when it is the longer (`HL_SERVER=every` gives every frame, `inline` runs it on core 0). The packet's entities are
packed by four parts on three packers and the server's helper (`SE_PackJob` in `xash3d.patch`); the client and the server step their entity rings by whole
blocks, and the light styles skip the time of styles not used.

A paced server leaves several of the client's commands unanswered, so the client's ring of updates is 16 under `XASH_LOW_MEMORY`, where 4 would stop the
prediction at 3 unanswered (`CL_InitEdicts` in `xash3d.patch`; the server's stays 4), and the client's packets of a frame out go whole to the server's next frame,
where the loopback, which keeps the last 4, would lose the older ones (`SE_ServerTakePacket`; `HL_INBOX=loopback`).

A packed file (a map, a model, a sprite) is not opened by a worker's job: `FS_OpenInflated` refuses it there and the open fails, and opens it on core 0 for the
server's worker and the drawing's. A level change the game makes in the server's frame (a `trigger_changelevel`: hldemo1's elevator) is therefore a `changelevel2`
command for core 0 (`pfnChangeLevel`), which reads the next map with the server settled.

A studio model's sequence groups (`scientist01.mdl` to `scientist07.mdl`, `barney01.mdl` and the like) are loaded with the model by core 0 (`SE_StudioGroupsLoad`, from
`Mod_ForName`): the engine and the client's renderer would load one where it is first played, the server's worker or the drawing's. Those run on stacks of their own, which
`mcw_on_worker` does not know, so a file such a worker opens is opened by core 0 (`FS_OpenInflated` asks through `SE_ServerOnCore0`), and `SE_MediaWorkers` gives no workers off
core 0, while the rings are on or while core 0 makes the server's ask: a decode there is core 0's alone.

A change to the game is also walked: `rvc_cpu` with the card's model on the desktop (`--desktop`, `RVC_SOUND`, `RVC_WCACHE`, `RVC_ADFAULT`, `RVC_RACES`, `--ips 3100000 --pass-ms
0.2`) and an `HL_SCRIPT` of `+forward` with a turn every 4 to 8 s, shots, jumps and use, for 1,200 s of the guest's time: standing in the fight never plays a sequence group. The level's
clock must not stop, and 0 undone. Statics both the server's worker and another core write are each core's own (`SE_Who`): `Info_ValueForKey`'s values, the PVS's `g_visdata`, and the random
generator's state; what a worker writes lies in texels apart (`SE_APART`), and `RVC_RACES` with `RVC_RACES_NEAR=4` finds such texels (`docs/machine/multicore.md`, "Passes apart"). The renderer's
lines to stderr write by themselves (`hlglSay`).

The studio models' hit boxes are hulls of each core's own (`studio_hull`, `studio_of[3]` in `mod_studio.c`), made by the core that first traces against one (`Mod_HullForStudio`); the box hulls of
`pm_trace.c` are each core's too, or the server's worker traces its bullets against hulls with no planes. A job of the light ahead (the glows' `SE_VisTraceFraction`) has a box hull of its own too (`box_of[3]`).
Two glock shots of 8 kill the headcrab after the glock in hldemo2 (`HL_SCRIPT`, `ent_info 530`).

#### A level that goes, and the sequence groups after it

To run the elevator from hldemo1 to hldemo2 on `rvc_cpu` with the card's model (the walk's switches, `HL_SCRIPT` with `-dev 1`: `ent_fire 1 set origin`, `ent_fire startdometalk_mm use`, the elevator's
`startele1` used; `ent_fire` wants `+sv_enttools_enable 1`, or the console says `Unknown command: ent_fire` and nothing happens, and the player must be inside `hldemo1to2`, as at `-2530 -160 200`: `startele1`
opens the doors, and used again closes them, which is the change). A change here is run through the elevator and `changelevel`s each way, with the default and `HL_RENDER=core`, 0 undone.

- A level that goes waits first for the server's frame, the drawing and every job (`SE_LevelSettle` in `rnd_shaderemu.c`, from `COM_Frame` whenever the host's state is not a running frame).
- Whether a sequence group is loaded is its pointer off core 0 (`Mod_CacheCheck`), not a walk of the studio cache's pool, whose list and the pool table core 0 may be changing (a pool made moves the
  table). A group a worker still finds missing gives the first sequence's pose and says `a worker wanted ...`.
- Statics of core 0's and a worker's do not share a texel: `serving` (core 0's, whether it makes the server's ask) and the server's count of asks (`se_asks`), or a clear of `serving` is undone and
  `Cbuf_Execute` runs commands with the server's frame out; the drawing's `pulled` vertices and `se_settle_passes`.

The level's clock runs slower than the guest's where the server's frames are long (the server's frame given at most 0.25 s, `MAX_FRAMETIME`), so the player there can outrun the game's people.

Decals and brush models ask for `glPolygonOffset`, which the GPU has no state for: with `GL_POLYGON_OFFSET_FILL` on `gl_shaderemu.c` moves each point toward the eye along its own ray, by (factor + units)
times 4e-4 of its distance (a decal's (-1, -2) is 0.12%), so it covers the same pixels and only its depth changes.

### A move predicted once

The states lie by the command (`cl_pmove.c`, `cl_parse.c`; a state is 2.3 KB, the weapons up to Half-Life's id 15), and a frame runs only the commands not run yet: the moves before them say the same while
nothing came from the server, and when something did, they say the same while it says what was predicted for the command it answers (`SE_AsPredicted`: place within 0.05, speed within 1, every state that a server
may change by itself exactly). Then what the server said replaces the state kept for that command, near as they are: a new move starts as every move again's, since 1/256 of a unit at a step's edge is whether it
climbs the step. Otherwise it is every move again. `HL_PREDICT=full` runs them all each frame; `HL_PREDICT=check` runs every move again beside the states kept and prints how far they are apart (`hlpredict:`): 0 over 0.1
in a walk with turns, jumps and shots, and few replays (a move that pushes the server's way, a teleport).

### What core 0 did between the drawing's end and the next give

A frame is as long as the drawing's job and what lies between its end and the next job's start, or as core 0's frame if that is longer: core 0 waiting at the swap does not shorten it. `HL_RENDER_CALLS=1`'s timeline
shows that span. To keep it short:

- The entities' copies are made at the frame's end while the drawing still draws, into the other of two sets (`R_SceneAhead`; `HL_SCENE=give` is at the give), and what its end writes back to the entities (attachments,
  the frame it was at, a muzzle flash shown) goes to the new copy too (`R_SceneTakeOne`). A cache of a copy's address breaks on the two sets: the glows' fractions go by the live entity (`R_SceneLive`).
- A paint of the sound is once a frame, or an eighth of a second after the last in a long frame (`S_ExtraUpdate`: the card plays by itself).
- Only the lights that are lit are copied to the drawing (`SE_LightsGive`), the rings' statistics are read only under `HL_RENDER_CALLS`, the bones' and light's slots are cleared by the keys kept and not whole, and the
  view's own player is not interpolated in first person (`CL_ComputePlayerOrigin`; a demo or a third-person view still is).

A cache of a model's meshes by submodel and skin gives no frames.

## The controls

On a desktop the left button held drags the view round, a quarter of a degree a pixel, as Quake's and Quake III's; the right button fires and the middle one is the second fire. The pointer's travel is read from the machine's
input words (`Platform_RunEvents`), and the window system is asked for events only when a button or a key changed and four times a second. Use is E (the demo's `default.cfg` has Tab: `data.py` moves it). The keys are
`default.cfg`'s: the engine runs it when there is no `config.cfg`, as GoldSrc does (`host.c` in `xash3d.patch`; `config.cfg` is written under `/tmp` and gone at the next start).

## From the seat

Played from the holodeck's seat the game reads the host's words (`docs/world/holodeck.md`, "Played from the seat"; `SE_SeatMove` and `SE_SeatView` in `vid_shaderemu.c`): the eye is 64 units over the floor, the sticks walk and
turn, the right trigger fires, the left is the second fire, the right grip (E on a desktop) is use, Space jumps; with tracked hands the right hand aims and the view model is held in it; with a mouse it turns the view, 0.066 degrees
a count. The picture drawn round the seat leaves no surface out (`r_nocull`). `HALFLIFE_SEAT=1` prints how it is seated every 20 frames, and `rvc_cpu --host 00000112,0,0,0,0,00007f00,0,0` walks a desktop's sitter forward.

With tracked hands the shot leaves the hand along its beam: `SE_SeatMove` sends where the hand is from the eye in the command's `impact_index` (1) and `impact_position`, the game's `GetGunPosition` adds it and `GetAutoaimVector` gives
the view's way with no autoaim and no punch, and the client's events start from the same place (`EV_GetGunPosition`, `hlsdk.patch`). The view model points the aim's way, none of the client's bob, sway or autoaim on it, held a third of its
length from its back with its muzzle (attachment 0 at rest, `SE_StudioAttachmentRest`) on the beam: `HALFLIFE_SEAT=1` must say the muzzle is 0.00 off the beam, and shots from `--host 80000033,0,0,0,0,0,064ed44b,0e391555` leave the hand.

`HL_SCRIPT="T:command|T:command"` runs console commands at a level's second T (`script` in `vid_shaderemu.c`), `look P Y` sets the view's angles and `turn D` turns D degrees a second: with `rvc_cpu` it is how a fight is played again, run
after run.
