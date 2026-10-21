# Seeing each other's machines

In the VRChat world every player runs a machine of their own. A player can put another
player's console and display on the screens in front of them, and, when its owner allows it,
type and point at it. Nothing of the machine itself is shared: only what its screens show.

## What a player does

- The panel between the display and the memory view asks "Whose computer is on these
  screens?": a row for "Automatic", one for "My own", and one for each other player with
  whether their machine is on and may be used, and how many have it on their wall (eight a
  page, "More players" with the page for the rest). Clicking a row picks it; the panel says
  what is shown, how many watch yours, and under the rows who watches the machine shown.
- With another player's machine on the wall, everything of the den is theirs: the console,
  the display, the control panel's read-out with core 0's registers, the wall's speed
  window, the den's tower's lamps.
- Automatic (the start): your own machine while it is on; while it is off, the machine of
  the first player (lowest player number) whose machine is on.
- "Others may use my computer", on the same panel, lets whoever is watching yours type on your console and use
  your display with their keyboards and beams. It starts at no.
- While another player's machine is shown, the keyboards and the beam on the display are
  theirs (if allowed) or nobody's: nothing typed then reaches your own machine.

## How it travels

`EmuShare` is a VRChat player object: one a player, owned by that player, synced by hand
(`RequestSerialization`). `EmuShareHub` is local, one a client: it fills this player's
object. `EmuStreams`, also one a client, takes what the others' objects bring, for every
screen that shows a player: the wall, a classroom tube (`docs/world/stations.md`), a holodeck's
screen. A player sends console and display while their machine is on and anybody else is in
the world, shown or not, and every client keeps what every sender brings: a screen that
turns to a player has their picture at once (it started from nothing, and took seconds, when
only watched players sent; in a full classroom they all did anyway). It goes out on two
ticks of three (6.7 packets a second) of at most 1,150 bytes: 7.7 KB/s, and the
other fields beside it. VRChat's limit is for everything a client sends, whatever the
object: 11 KB/s on paper, 8 to 10 in practice.

| Synced field | Contents |
|---|---|
| `flags` | bit 0: the machine is on; bit 1: watchers may use it |
| `watching` | the player whose machine this player has on the wall, 0 for their own |
| `seq`, `packet` | a packet's number and its bytes (empty when only the other fields changed) |
| `inputSeq`, `input` | key events for the watched machine: a count and a ring of the last 16 |
| `pointer` | x and y in the watched display's pixels (12 bits each), buttons << 24, present << 28 |
| `station`, `stationMode` | this player's classroom place and what its tube shows |
| `ask` | sixteen words, one for each sender this player keeps: the sender's number, and a count << 16 that goes up with each asking |
| `lost` | beside each: what is asked for again, a packet number's low 16 bits and how many from it << 16; none: everything |
| `holo`, `holding` | the holodeck this player sits in, and whether the controller is in their hands |

The network's packets go in a player object of their own, `EmuNet` (`docs/system/lan.md`); the
hub keeps the count of a player's batches in their `EmuShare.netSeq`, which is not synced.

A packet: 19 bytes (on, the console's first row, cursor column and row, the stream's width
and height, the display's width and height, the number of rows, the machine's speed:
tenths of a million instructions a second for all cores and for core 0, two bytes each, and
the length of a status), the status if any, the console rows, then the display's units to the
end ("Display", below).

The status goes every two seconds while the machine is on (`PutStatus`): sixteen bits of
activity (`EmuMachine.activity`: one an eighth of a second, set where core 0's registers had
changed, which is the red disk lamp's flicker on every tower), pc and x1 to x31 as the last
readback had them, and the control panel's text (`statsLine`, at most 560 characters). It is
not kept for repairs: the next one comes anyway.

- Everything in a packet is state, not a difference: a row or a unit replaces what the
  receiver had.
- **A slot a sender.** `EmuStreams` has sixteen slots: a picture, a console.
  Every other player whose machine is on has one while slots last, and is drawn once
  however many screens show it. Each screen says whose it wants (`SetWant`): a sender a
  screen wants and that has no slot takes one from a sender no screen shows. A slot whose
  sender left, or is off and not shown, is emptied and its picture made black.
- **A sender knows who keeps it** from the others' `ask`: a word that names it. A new word is
  a new receiver, and the sender sends everything (at most once in five seconds, whoever
  asks).
- Who watches whom is `watching` alone: the rows' counts and the names under them are
  counted from it (`Watchers`, `WatcherNames` in `EmuShareHub`).
- **A lost packet is repaired, not the whole picture.** A receiver that sees a gap in `seq`
  names the missing numbers in `lost` and counts its word up. The sender keeps which console
  rows each of its last 32 packets carried and marks those as not sent; of the display, every
  cell carries the number of the packet that brought it, and the cells with a lost number go
  stale in the next packet made. A gap older than 32 packets is everything again.
- A slot whose sender is on and has brought nothing two seconds after it was made asks for
  everything, and again every five seconds: so a screen never waits for its owner's picture
  to change.
- A late joiner gets the last packet from VRChat, makes its slots, and is a new screen.
- A player's object is destroyed with the player. The hub takes it off its list in
  `OnPlayerLeft` and again (`Prune`) before anything reads the list: reading a destroyed
  object halts an Udon behaviour for the rest of the session, which is what froze the
  classroom's tubes in October 2026.
- `EmuStreams` writes a line a minute to VRChat's log (`[Streams]`): packets, gaps, repairs,
  refreshes, and each slot's sender, last packet and how long ago it was heard.

## Console

`EmuTerminal` notes which rows of its grid were written. A row travels as: row number,
length without the blanks at its end, the characters, then 0, or 1 and a byte of colours a
cell. A typed character is one short row; a full screen is about 1.2 KB.

## Display

The GPU makes the whole of a packet's display part (`ShareUnits.shader`); Udon copies one row
of bytes, and the receiver's GPU draws it (`ShareUnitsDecode.shader`). "ShaderEmu/Update the
display stream in the open scene" makes its textures and fills the hub's and the receiver's
fields. Until 13 October 2026 the display went as tiles on a grid of 32x32 (64x64 of the
display's pixels where the stream was half its size), each sent coarse and then sharper, picked
by Udon: "Judged while it moves", below, is why it does not any more.

- There are no differences between frames. A unit is coded from the current picture alone and
  replaces what the receiver had there: losing a packet costs sharpness for a moment, never a
  wrong picture.
- The picture is the display's own pixels (divided by a whole number when it is over 1280 x
  960), cut into blocks of four sizes: 4x4 samples of 1, 2, 4 or 8 pixels (levels 0 to 3),
  two colours (5:6:5) and a bit a sample. A block lies on its own level's grid, not on tiles.
- A unit is 4x4 blocks of one level, on that level's own grid, with a bit a block that is
  there: 16, 32, 64 or 128 pixels square. Its record: two bytes of level and number, two of
  bits, two bits a block of kinds, then the blocks, repeated colours left out as in a quarter.
- The sender keeps, in a texture, for each 8x8 cell the level the receivers have it at (4:
  stale) and the number of the packet that carried it. A cell that changed since the last
  capture is stale. A cell needs the coarsest level whose block shows every pixel as level 0
  does: a flat area goes as a few large blocks, text at level 0.
- A packet: stale cells first, as sharp as one packet can carry all of them (what keeps
  changing is then whole in every packet, if coarse), in the room less a quarter; then the
  cells that are coarser than they need, as sharp as two packets can carry them. A cell goes by the coarsest block that spoils nothing
  beside it. Units go in an order (stale before the rest, coarse before sharp, each level round
  the screen from where the last packet stopped) and the packet is that order's start, cut
  where the room ends: which units go is a sum, so the GPU can pick them.
- The passes: blocks, exact, need, marks, stats, plan, units, prefix, cut, packet, state. They
  cost about 0.4 ms a packet together in `share_test --time`.
- A tick that may carry the display allows one packet. It is asked for the frame the link is
  free (its header and console rows then) and goes the frame its units have been read back
  (`AskUnits`, `TakeUnits`, the start of `Update` in `EmuShareHub`); one that waited for the
  link is made once more of the display as it is then.
- A lost packet: the cells that carry its number go stale in the next packet made. Everything
  again: all of them do.
- The receiver keeps no store: a pass finds each unit's record in the packet's bytes, a second
  draws the pixels those units cover into the slot's picture, the sharpest unit where two
  overlap (`ShareUnitsDecode.shader`).
- `tools/share_units.py` is the same in numpy, pass for pass, and `bin\share_test.exe` runs the
  two shaders outside Unity on pictures of a display: `share_test A.png B.png --out FOLDER`,
  then `python tools/share_units.py A.png B.png --against FOLDER` must say every packet has the
  same bytes, the same state after it and the same picture (`--lost-at N` for a lost packet).
  On 13 October 2026: 656 packets of four runs under fxc2 and under Windows' FXC, all the same.
- In a model of both senders on the desktop at 800 x 600 (not in Unity): a letter typed, 4
  packets and 3,766 bytes as tiles, 1 and 372 as units; the editor opening, 54 packets against
  15; the browser opening, 91 against 38.

## Judged while it moves

The stream is right when the picture a receiver shows is close to the display while the
display keeps changing: a window dragged, the pointer, windows with activity, text scrolling,
a game. Bytes and the time to a settled picture do not say that. Three measures, each of
the tiles that were sent before against the units that are sent now (one build had both, a
switch between them); lower is better in all.

**Two real clients** (the one that counts). With `netTest` set on the hub, "Build & Test" with
two clients: the first client's machine shows five scenes, 80 s each (in the build that had
both ways it sent each as tiles and as units, 40 s each, the order turned round the second
time through); `EmuShareProbe`
writes 24 x 18 pixels of the display (the first client) and of the picture of it (the second)
three times a second into each client's log with the instance's clock, and the hub and the
receiver write when every packet's picture was captured, sent and received.
`python tools/share_clients.py LOG_A LOG_B` holds the two logs together. Turn `netTest` off
again before saving the scene. On 13 October 2026, 4,823 packets, none lost, 5 a second of
1,050 bytes either way, the mean difference (levels of 255) between the watcher's pixels and
the display's at the same moment, over the pixels that change in the scene:

| Scene | tiles | units |
|---|---|---|
| Doom's first demo | 23.5 | 21.7 |
| glxgears and the system monitor | 21.0 | 12.9 |
| a terminal whose text scrolls | 20.6 | 16.6 |
| a window dragged about | 58.3 | 59.2 |
| the pointer going round | 20.0 | 12.0 |

A packet's picture is 208 ms old when it goes as tiles (9 in 10 under 322), 42 ms as units
(under 60). The game and the dragged window are level: there the link's 5 KB/s is what is
short, whichever way it is spent.

**The editor's self-test** (`selfTest`: this client's own packets through its own receiver,
the receiver's picture read back five times a second against the display, 30 s a case; the
hub's `sending` cleared every frame for VRChat's rate, left alone for the editor's one a
second). Over the changing pixels and 8 round them, 12 October 2026, before the last two
changes to the order (below):

| Scene | 6.7 a second | 1 a second |
|---|---|---|
| a window dragged | 11.7, 10.7 | 15.5 and 14.0, 14.9 and 10.2 |
| the pointer | 13.1, 7.1 | 12.8, 7.1 |
| glxgears and the monitor | 21.3, 20.0 | 26.2, 20.9 |
| a terminal scrolling | 11.8, 6.5 | 14.9, 8.8 |
| Doom | 20.1, 17.0 | 25.3, 24.8 |

Twelve seconds of a game at a packet a second, three times each, were 34.3, 27.8 and 25.1 as
tiles and 19.1, 28.3 and 20.4 as units: one short sample says little.

**A model** of both senders on recordings, for trying a change before it is in Unity.
`rvc_harness --cpu --cpu-ips 3000000 --desktop ... --screen-log SCENE.scr --screen-every 33
--screen-frames 300` records the guest's display at the world's speed (`docs/machine/cpu-harness.md`);
`python tools/share_motion.py SCENE.scr ... [--video FOLDER]` plays it through each sender on
the recording's own clock, a packet every 0.15 s and every second, and prints the mean
difference between what the receiver shows and the frame current at each moment, over the
pixels that change at all (`--video`: what each receiver showed, as .mp4). Five recordings of
300 frames:

| Scene | of the screen changes | every 0.15 s | every second |
|---|---|---|---|
| a window dragged about | 77% | 23.7, 17.8 | 48.3, 41.0 |
| the pointer over two windows | 1.7% | 2.56, 2.33 | 3.03, 2.78 |
| glxgears and the system monitor | 5.5% | 28.2, 5.1 | 30.0, 12.8 |
| a terminal whose text scrolls | 24% | 13.2, 8.9 | 13.9, 10.2 |
| Doom's first demo | 66% | 16.9, 11.9 | 34.1, 18.9 |

The model flatters the units where the link is short (the game, the drag): between real
clients those two are level. What made the difference, in the order it was found:

- A packet goes the frame it has been read back, not at the next tick, and one that waited
  for the link is made once more before it goes: its picture 40 ms old, not 200 (or a
  second, in the editor).
- The stale cells go as sharp as one packet can carry all of them, not two: a window being
  dragged was half a sending behind.
- Each level's units go round the screen from where the last packet stopped. They began at a
  place that jumped about with every packet, and with a game filling every packet some parts
  waited several packets: between real clients that was worse than the tiles (11.8 against
  8.5 in a coarser measure of the same test).
- A quarter of a packet is kept for the cells that only want sharpening: with a game on, the
  rest of the screen stayed as coarse as the last full sending had left it.

Not measured: a lost packet's repair in Unity (the shader's side of it is in `share_test
--lost-at`), and VR clients.

## Using the watched machine

The hub takes what the keyboards queued and the beam's place on the display, and sends them
in its own object; the owner's hub hands them to `EmuMachine` (`RemoteChar`, `RemoteKey`,
`SetRemotePointer`), which reads them beside its own keyboards. When both the owner's
beam and a watcher's pointer are on the display, the one holding a button has the machine,
else the one that moved last: a desktop player's beam is the middle of their view, so it is
on the display whenever they look at it. A pointer is repeated twice a second while it is on
the display and dropped by the owner after two seconds of silence.

## Testing

- `selfTest` on the hub ("Share" in the scene) shows this player's own machine through the
  stream: set it in play mode with `SetProgramVariable`. Compare the slot's picture
  (`StreamPicture0`) with `ShareFineA`/`B`, and its cells with the terminal's `grid`.
- Several players with one client: `ClientSimMain.SpawnRemotePlayer`, and an editor hook
  that gives each new `EmuShare` a `station`, copies this player's `seq` and `packet` into
  it when `seq` changes, mirrors this player's own `ask` and `lost` words back (with this
  player's number in place of the sender's) and sends it `_onDeserialization`. `dropTest`
  on `EmuStreams` loses every tenth packet: its `repairs` and the hub's `repaired` must go
  up and the hub's `refreshed` must not.
- The scene has one "Players panel", under the den's Computer, and every button of it calls
  the hub. On 13 October 2026 it had two in one place: "Add sharing" had found the model
  Models/Computer by name, left the old panel (its hub gone, its buttons calling nothing)
  and hung the new one on the model; the old one took the clicks. "ShaderEmu/Remove the dead
  players' panel in the open scene" mends such a scene; the den is now looked up as the
  world's own child (`Den()`).
- Destroy a spawned player's "Share player" object before its `OnPlayerLeft`: no behaviour
  may halt (`enabled` stays true), and the place reads FREE with a black picture.
- ClientSim never raises `OnPostSerialization` for these objects, so `EmuShare.Busy()` runs
  out its one-second limit there: a packet a second in the editor, six or seven in VRChat.
- Two real clients: set the hub's `netTest` in the scene, set the SDK's number of clients
  to 2 and "Build & Test". The first player in the instance turns their machine on and
  allows use; the second watches it by itself and, 45 s later, types `echo NET-2-OK` at it.
  Every client writes a `[ShareTest]` line every two seconds to its own
  `LocalLow/VRChat/VRChat/output_log_*.txt`: packets and bytes sent and received, gaps,
  and the `NET-` lines on the console it shows. Nothing has to be fed
  in or read off the screen. Turn `netTest` off again before saving the scene.
- That run (2026-10-07, both clients on one PC): 133 packets, 143,881 bytes in 27 s, about
  5 a second and 5.3 KB/s, none lost, never clogged; every tile had arrived coarse 4 s after
  the second client started watching, and the typed line came back in the stream.
