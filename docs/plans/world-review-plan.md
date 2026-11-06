# The world and its builder: a review, and what to fix (6 November 2026)

A review of every runtime script (`unity/ShaderEmu/Scripts`) and of the builder (`unity/ShaderEmu/Editor`)
for what a visitor meets: on a desktop and in a headset, alone and with others, joining late, sitting,
typing, saving, browsing, leaving. Both scenes were checked in the editor: no missing references
(the empty fields are filled at run time or are meant to be: the ROM's slot 0, the paste fields'
keyboards), every one of the 748 asset ids in each saved scene file resolves (the builder's assets made
again keep their ids, so a build in one scene does not break the other), the spawn inside the corridor,
the gate's 17 parts, no errors in play mode.

## Fixed

| What a visitor met | Where | Checked |
|---|---|---|
| A desktop visitor typing at a classroom keyboard stayed frozen, and their keys went on into the machine, when the place went other than by EXIT (the machine switched off, an earlier visitor's claim) | `EmuStations.Assign` lets the old place's keyboard go | play mode: place 0 taken, typing on, power off: the keyboard lets go |
| A holodeck seat stayed up under nobody for everyone else when its sitter left the instance seated | `EmuSeat.OnPlayerLeft` | play mode with ClientSim's remote player: seated (0.23 m up), removed: the seat is free |
| A guest program could stop the console for good: `ESC[99999999999B` overflowed the number, a negative count put the cursor off the grid, and Udon stops a behaviour at a bad index | `EmuTerminal`: an escape's number is 9,999 at most | play mode: the sequence printed, the console went on |
| Another client's synced arrays were read at the lengths this client expected: a shorter one (an object of an older template, as the code says it has met, or a modified client) stopped the hub, and with it every screen's stream | `EmuShareHub.Scan`, `TakeInput`, `Roster` | compiles |
| An answer VRChat brought after its 20 s were up was taken as the answer of whatever was loading next: a page's pictures queue at one load in 5 s | `EmuMachine.Held`: each of the four load events looks at its address | play mode: a page of the world's list loads (2,217 bytes) |
| The storage sign said "Saving..." for ever when the folder was too large: `nxpersist` refuses it on the desktop and never asks the host | `EmuPersist.Save` says "the desktop says how it goes"; the host's own refusals and a failed readback say "Not saved" | play mode: a small save says "Saved."; 150 KB of random bytes: the desktop's `nxpersist` was still compressing harder after two minutes |
| In a headset, a right stick's scroll let a visitor walk away from a keyboard typing for them | `EmuPointer.Still` keeps them still while any keyboard captures | compiles |
| The holodeck made an array of 16 words every frame (garbage in Udon) | `EmuHolodeck.Tell` keeps one | compiles |
| With no setting, the builder read `C:/Development/ShaderEmu` (AGENTS.md: no default path) | `ShaderEmuImages.Repo` asks for the folder once, checks it has `shader_vm` and `AGENTS.md`, keeps it | the setting of this editor is kept; a wrong folder is refused |
| "Restyle the panels in the open scene" left the holodeck's keyboard in the old style and the touch keys stale | `RetroOpenScene` makes what `Build()` makes after `Retro` | compiles; not run (the open scene had unsaved changes) |
| Every doorway of the two corridors (the three side rooms', the eight holodecks') had the wall's 20 cm of no floor: a 3 cm dip on every crossing for a player's capsule, and a pen dropped there fell out of the world | `Annex`: the corridors' floors run under their walls | a walk of the floor from the spawn, 0.25 m a step, with the new boxes put into the scene and taken out again: every doorway has floor |
| N13 is 0.8 m wider than the corridor's west end, and its north wall had no collider there (its north wall is the corridor's): a visitor walked through the wall they saw, out of the building, and fell | `Annex`: a side room's north wall where it reaches past the corridor | the same walk: 0 edges to nothing, against 3 before |

| The window probe's pictures put every renderer but the city's away and brought them back after; a capture that threw (the card, memory) left them off and the probe's camera in the scene, which the build's save then kept | `WindowProbe`: what is put away comes back in a `finally` | compiles |

| Every Build world said "THE WINDOWS' GLASS HAS NO REFLECTION PROBE WINDOW" though it had just made it: the glass's watch kept the probe it had found, which the build destroys, and looked before Unity said the hierarchy had changed | `ShaderEmuGlass.Watch` looks for the probe again before it complains | compiles; the probe and its five panes were there after both builds |

On 6 November 2026 both scenes were built again and baked with these fixes (`ShaderEmuWorld`, `ShaderEmuWorld Copy`), after a
build on a throwaway copy: there no probe camera was left and no renderer was left off (18 are off on purpose: the pens' markers,
the beams, the gather mesh), "Restyle the panels in the open scene" made the holodeck keyboard anew with every touch key (288 buttons,
65 sliders, none lost) and the pointer holding the new keyboard, and the lines of sight said 0 covered. Each scene: every floor from
the spawn reachable, no edge to nothing, six reflection probes baked (the lightmaps were not: see below); in play mode the desktop, the storage sign and a holodeck seat
with its keyboard, no errors.

The two collider fixes are the builder's: a scene has them after its next "Build world" (and a bake).

Also looked at, and as they should be: no real-time lights or reflection probes, one scene camera (the machine's are drawn by
hand), every sound in 3D but the machine's own; no hole in any floor; the eight seats hold their sitter (VRChat's own exit off:
jump, End, both grips or the console's Exit stand them up, and a respawn always does); the packets of others are read within
their bounds (`EmuStreams.Received`); the hub's batches are held back by `Networking.IsClogged` and the event's own limit;
nothing a script moves is batching-static (the holodecks' screens, marks, mask, world, HUD and keyboard, the seats, the
classroom's fields, the links panel, the controller's notice, the pointer's dots and beams, the keyboards' marks: 31 roots and
374 transforms the pointer and keyboards hold), so none is frozen where the bake left it as the links panel was; the keys' caps
sink in their shader by world-space vectors, which batching leaves alone.

Later the same day, fixed and tried:

| What | Where | Checked |
|---|---|---|
| A bake's end (the lamps off, the room's probes, the save) waited on `EditorApplication.delayCall`, which an editor in the back never runs: the lamps stayed on and the scene unsaved until the editor was next in front, and then it ran on whatever scene was open | `ShaderEmuGlass.Soon` (a one-shot `EditorApplication.update`) for the bake's end and the glass's own checks | a `delayCall` did not run in 14 s in the back, `Soon` at once; the v2 scene's bake then ended and saved by itself |
| Every build logged four "Not allowed to access uv" errors for "Room shelf things" | the object is empty (`world/rooms.py` names it and puts only assets of their own on the shelf): `TinyChartShare` skips a mesh with no vertices | the build after it logged none; the check still measures the others (the elephant 78%) |
| Every build made a city of boxes, two meshes, a material and two pictures, and `Remodel` threw the city away at once | `City()` and its three helpers are gone from `ShaderEmuDecor.cs` | the build after it is whole, its window shows `Models/City.fbx` |
| A keyboard that stopped typing while a stick scrolled let a headset's player walk for that scroll | `EmuPointer.Still` holds them again when a keyboard lets go under a scroll | compiles into Udon |
| "Add the Udon machine to the open scene" did not say a rebuild takes it away | its log says it is an experiment and not a part of Build world | compiles |

The v2 scene was built once more by these (a build meant for a copy ran on it) and baked again. **Neither scene has its lightmaps
now**, though the bakes said they were done (what was written here before, that both were lit, was wrong: the pictures were the light
volumes'). In the editor that ran today's bakes Bakery's own programs never started (`ftraceRTX9 launch failed`, `halffloat2hdr
launch failed`, 222 times in its log, from the first bake on): in that editor `frender.dll`'s `RunLocalProcess` started nothing,
while .NET's `Process.Start` started the same program, and so did `RunLocalProcessVisible` (a window of its own); giving the editor
standard handles (NUL) did not change it. Why is not known: the editor had been started from a Claude Code session, but one
started from Unity Hub starts Bakery's programs (`halffloat2hdr`, `ftraceRTX9`) where that one did not. A restart of the editor
was the cure. The previous editor's three bakes had no such line. Bakery ends its render all the same, so the builder saved
both scenes with a store whose 309 renderers are all empty: no renderer gets a lightmap in play mode. `BakeryDone` now counts the
store's renderers (the store's own list, which a whole-scene bake fills, and its sectors') and, with none, says so and saves
nothing; on its first bake it counted only the sectors and refused a good bake, which was then saved by hand.

After a restart of the editor from Unity Hub both scenes were baked again (22:23 and 22:26): each has its own three lightmaps
(`ShaderEmuWorld_LM0`, `LMA1`, `LMA2`, and the copy's), its store names 311 renderers after the scene is opened again, and in play
mode 311 of its 632 renderers have a lightmap, with no error and the scene unchanged after play. The second bake saved by itself
("Bakery lit 311 renderers"). Left in the project, which the permission check would not let be deleted: `Generated/City.mat`, `Generated/WindowCity.mat`,
`Generated/CityStreets.asset`, `Generated/CityTowers.asset`, `Textures/CityStreet.png`, `Textures/CityWindows.png` (nothing refers to any).

## To do

1. **Two real clients, then upload.** Two Build & Test clients with `netTest` and the gate's `testOpen` (`docs/world/share.md`),
   three runs. The first found a fault no editor run shows: in both clients LTCGI's adapter said "No screens found!" and Udon halted
   it, so no screen lit the room. The gate's build step switches the computer off, the screen's `LTCGI_Screen.OnDisable` asks LTCGI
   to count its screens again, it finds none active and writes 0 into its adapter's proxy, and UdonSharp ships the proxy.
   `ShaderEmuGateBuild` now sets LTCGI's `Singleton` aside while it switches the parts off, and `Build()` writes LTCGI's data into
   the adapter's Udon program before it saves (`LtcgiIntoUdon`). The third run: "LTCGI adapter started for 1 (0 dynamic) screens,
   214 renderers" in both, no behaviour halted, the second client's `echo NET-2-OK` on both consoles, 1,330 packets sent and 1,285
   taken with no gaps over 290 s; every other error line in the logs is VRChat's own (its API's avatars and inventory). Both scenes
   were saved with LTCGI's data, the test switches off and the SDK at one client again. Left: the upload (the developer's), and a
   sitter quitting and `nxweb` with many pictures, which these clients do not do by themselves.
2. **Play mode marked a scene changed.** Done: the saved scenes had no network ids for four synced objects the builder makes
   (the seat, the stations' fields, the streams, the weather), and ClientSim gives them as play mode starts
   (`ClientSimNetworkingUtilities.DoSceneSetup`), so every play left the scene changed. `Build()` now calls the same before
   it saves; both scenes were given their ids and saved (nothing else changed in them). With Unity's own play and stop the scene
   stays unchanged; the Unity MCP's play command still marks it, which no visitor meets.
3. **The gate turns 17 parts on in one frame.** Leaving the corridor is one long frame (every behaviour's `Start`, the
   machine's textures, LTCGI). Measure it with the profiler in a client; then a few parts a frame, the machine first.
   A floor from the editor (play mode, the 17 parts put off and on again in one call): the `SetActive`s took 14 ms (the stations
   8 of them) and the next frame 40 ms, then 8, 4, 3. That is without what only a first start does (each behaviour's `Start`, the
   machine's textures made, the scripts' programs loaded), so a client's frame is longer; only its profiler says by how much.
   In the clients the parts came on 5 s after arrival with nothing halted (since the LTCGI fix); the frame itself is still unprofiled.
   `EmuGate` now logs it once: "[Gate] the parts' SetActive: N ms" and the four frames after; the next client run says how long.
4. **The holodeck steps run twice in a build** (`Annex`, then `Build()` after `Retro`). Kept, as meant: `Annex` says why
   (a holodeck it makes again needs the Hide key, the show buttons and the keyboard, or `EmuHolodeck.Arrange` finds no plates,
   and `Retro` styles what is there), and each step takes away what it made before. The second run costs about 5 s of a build
   (`ConsoleSwitch` 4.8 s, `AddHoloKeyboard` 0.1 s, timed in the open scene, which was then discarded). Nothing to do.
5. **Sixteen stream slots for 24 surfaces** (`EmuStreams`): the wall, 15 places and 8 holodecks can want 24 senders;
   past 16 shown a screen was black without a word. Done for the classroom's places: `EmuStreams.NoRoom` says when every
   slot holds a machine that is shown; a classroom place's key then says "NOT SHOWN: 16 shown at once", and a holodeck's
   door "NOT SHOWN" over the sitter's name (the note first: the door's label is 300 x 80 at 30 points with no rich text, and a
   long name is what gets cut). Both texts were put into every label in the scene: none cut. Compiles into Udon; not run with
   17 players.
6. **The six city leftovers** above. Done: deleted, after a search of every scene, material, asset and prefab and of the scripts for them; both scenes have no missing reference.
