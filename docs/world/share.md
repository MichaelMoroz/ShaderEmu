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
turns to a player has their picture at once. It goes out on two
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
| `speed` | their machine's speed step (`EmuMachine.speedSlider`), which their tower's slider shows |
| `holo`, `holding` | the holodeck this player sits in, and whether the controller is in their hands |

The network's packets go in a player object of their own, `EmuNet` (`docs/system/lan.md`); the
hub keeps the count of a player's batches in their `EmuShare.netSeq`, which is not synced.

A packet: 19 bytes (on, the console's first row, cursor column and row, the stream's width
and height, the display's width and height, the number of rows, the machine's speed:
millions of instructions a second for all cores and for core 0, two bytes each, four digits and from
bit 14 how many come after the point (`PackSpeed`), and the length of a status), the status if any, the console rows, then the display's units to the
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
  object halts an Udon behaviour for the rest of the session.
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
fields.

### Units

- There are no differences between frames. A unit is coded from the current picture alone and
  replaces what the receiver had there: losing a packet costs sharpness for a moment, never a
  wrong picture.
- The picture is the display's own pixels (divided by a whole number when it is over 1280 x
  960), cut into blocks of four sizes: 4x4 samples of 1, 2, 4 or 8 pixels (levels 0 to 3),
  two colours (5:6:5) and a bit a sample. A block lies on its own level's grid.
- A unit is 4x4 blocks of one level, on that level's own grid, with a bit a block that is
  there: 16, 32, 64 or 128 pixels square. Its record: two bytes of level and number, two of
  bits, two bits a block of kinds, then the blocks, repeated colours left out.
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
- The passes: capture, blocks, exact, need, marks, stats, plan, units, prefix, cut, packet, state.
  `share_test --time` says what each costs.
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
  same bytes, the same state after it and the same picture (`--lost-at N` for a lost packet),
  under fxc2 and under Windows' FXC.

## Judged while it moves

The stream is right when the picture a receiver shows is close to the display while the
display keeps changing: a window dragged, the pointer, windows with activity, text scrolling,
a game. Bytes and the time to a settled picture do not say that. Three measures, each the mean
difference (levels of 255) between the picture a receiver shows and the display at that moment; lower
is better in all. A number from one means little alone: run a change and the tree the same way.

**Two real clients** (the one that counts). With `netTest` set on the hub, "Build & Test" with
two clients: the first client's machine shows five scenes in turn (Doom's first demo, glxgears and
the system monitor, a terminal whose text scrolls, a window dragged about, the pointer going round); `EmuShareProbe`
writes 24 x 18 pixels of the display (the first client) and of the picture of it (the second)
three times a second into each client's log with the instance's clock, and the hub and the
receiver write when every packet's picture was captured, sent and received.
`python tools/share_clients.py LOG_A LOG_B` holds the two logs together. Turn `netTest` off
again before saving the scene.

**The editor's self-test** (`selfTest`: this client's own packets through its own receiver,
the receiver's picture read back five times a second against the display, 30 s a case; the
hub's `sending` cleared every frame for VRChat's rate, left alone for the editor's one a
second), over the changing pixels and 8 round them. One short sample of a game says little.

**A model** of both senders on recordings, for trying a change before it is in Unity.
`rvc_harness --cpu --cpu-ips 3000000 --desktop ... --screen-log SCENE.scr --screen-every 33
--screen-frames 300` records the guest's display at the world's speed (`docs/machine/cpu-harness.md`);
`python tools/share_motion.py SCENE.scr ... [--video FOLDER]` plays it through each sender on
the recording's own clock, a packet every 0.15 s and every second, and prints the mean
difference between what the receiver shows and the frame current at each moment, over the
pixels that change at all (`--video`: what each receiver showed, as .mp4; `--only units` leaves out the
tiles, the older sender the model keeps as a yardstick). It takes over a minute on 150 frames. The model
flatters the sender where the link is short (a game, a drag): hold it against real clients there.

What the sender does for this:

- A packet goes the frame it has been read back, not at the next tick, and one that waits
  for the link is made once more before it goes.
- The stale cells go as sharp as one packet can carry all of them, so a dragged window is not
  half a sending behind.
- Each level's units go round the screen from where the last packet stopped: from a place that
  jumps about, parts of the screen would wait several packets with a game filling every packet.
- A quarter of a packet is kept for the cells that only want sharpening: with a game on, the
  rest of the screen would stay as coarse as the last full sending left it.

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
  the hub. "ShaderEmu/Remove the dead players' panel in the open scene" mends a scene with an old one
  left beside it; the den is looked up as the world's own child (`Den()`).
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
