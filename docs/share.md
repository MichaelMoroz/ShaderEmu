# Seeing each other's machines

In the VRChat world every player runs a machine of their own. A player can put another
player's console and display on the screens in front of them, and, when its owner allows it,
type and point at it. Nothing of the machine itself is shared: only what its screens show.

## What a player does

- The panel between the display and the memory view asks "Whose computer is on these
  screens?": a row for "Automatic", one for "My own", and one for each other player with
  whether their machine is on and may be used (eight a page, "More players" for the rest).
  Clicking a row picks it; the panel says what is shown and how many watch yours.
- Automatic (the start): your own machine while it is on; while it is off, the machine of
  the first player (lowest player number) whose machine is on.
- "Others may use my computer", on the same panel, lets whoever is watching yours type on your console and use
  your display with their keyboards and beams. It starts at no.
- While another player's machine is shown, the keyboards and the beam on the display are
  theirs (if allowed) or nobody's: nothing typed then reaches your own machine.

## How it travels

`EmuShare` is a VRChat player object: one a player, owned by that player, synced by hand
(`RequestSerialization`). `EmuShareHub` is local, one a client. It fills this player's
object and reads everyone else's. A player sends console and display only while somebody's
`watching` names them, at most five packets a second of at most 1,400 bytes: about 7 KB/s
of the 11 KB/s VRChat allows a client.

| Synced field | Contents |
|---|---|
| `flags` | bit 0: the machine is on; bit 1: watchers may use it |
| `watching` | the player whose machine this player looks at, 0 for their own |
| `resync` | counted up to ask the watched player for everything again |
| `seq`, `packet` | a packet's number and its bytes (empty when only the other fields changed) |
| `inputSeq`, `input` | key events for the watched machine: a count and a ring of the last 16 |
| `pointer` | x and y in the watched display's pixels (12 bits each), buttons << 24, present << 28 |

A packet: 13 bytes (on, the console's first row, cursor column and row, the stream's width
and height, the display's width and height, the number of rows), the console rows, then
display tiles to the end.

- Everything in a packet is state, not a difference: a row or a tile replaces what the
  receiver had. A receiver that sees a gap in `seq` counts its own `resync` up, and the
  sender marks every row and tile as not sent. The same happens for a new watcher.
- A late joiner gets the last packet from VRChat, starts watching, and is a new watcher.

## Console

`EmuTerminal` notes which rows of its grid were written. A row travels as: row number,
length without the blanks at its end, the characters, then 0, or 1 and a byte of colours a
cell. A typed character is one short row; a full screen is about 1.2 KB.

## What the display stream is, in short

- There are no differences between frames. A tile is always sent whole, coded from the
  current picture alone, and replaces what the receiver had: losing a packet costs
  sharpness for a moment, never a wrong picture.
- What is saved between frames is whole tiles: one that did not change is not sent.
- The GPU decides which tiles changed and codes every tile at all three sizes, every time.
- Udon decides which tiles and which size go into a packet, and copies their bytes.
- The bytes are not compressed further: no entropy coding, no run lengths.

## Display

Udon is too slow to touch pixels, so the GPU does all of it (`ShareEncode.shader`) and Udon
only copies bytes.

- Pass 0 draws the decoded display at the stream's size: the display's own size up to
  640x480, else divided by the smallest whole number that fits it in.
- Pass 1 packs that into a 128x300 target, a row for each 32x32 tile (20 across). A row
  holds the tile three times: 64 blocks of 4x4 pixels (384 bytes), 16 blocks of 8x8 (96),
  4 blocks of 16x16 (24). A block is six bytes: two colours (5:6:5 each) and a bit a sample
  saying which; the colours are the means of the samples darker and lighter than the block's
  mean. The larger blocks are cut from the capture's mipmaps.
- The row's last texels say whether the tile differs from the capture before (the last two
  captures are kept), and the coarsest size that still shows it exactly: 2 when its four
  quarters are each one colour.
- The target is read back (153,600 bytes). For each packet Udon picks tiles: those that
  changed, as sharp as two packets can carry all of them; when none is left, the coarsest of
  the rest one size sharper, never sharper than a flat tile needs. So a typed character
  arrives sharp at once, a moving picture arrives coarse at whatever rate fits, and a still
  one sharpens over a few seconds.
- The receiver copies each tile into the same layout (`ShareStore`, with the size it came
  at in the row's last texel) and `ShareDecode.shader` draws the picture from it into
  `RemotePicture`, which the wall shows in place of the machine's own.

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
  stream: set it in play mode with `SetProgramVariable`. Compare `RemotePicture` with
  `ShareCaptureA`/`B`, and the hub's `remoteGrid` with the terminal's `grid`.
- ClientSim never raises `OnPostSerialization` for these objects, so `EmuShare.Busy()` runs
  out its one-second limit there: a packet a second in the editor, five in VRChat.
- Two real clients: set the hub's `netTest` in the scene, set the SDK's number of clients
  to 2 and "Build & Test". The first player in the instance turns their machine on and
  allows use; the second watches it by itself and, 45 s later, types `echo NET-2-OK` at it.
  Every client writes a `[ShareTest]` line every two seconds to its own
  `LocalLow/VRChat/VRChat/output_log_*.txt`: packets and bytes sent and received, gaps,
  tiles at each size, and the `NET-` lines on the console it shows. Nothing has to be fed
  in or read off the screen. Turn `netTest` off again before saving the scene.
- That run (2026-10-07, both clients on one PC): 133 packets, 143,881 bytes in 27 s, about
  5 a second and 5.3 KB/s, none lost, never clogged; every tile had arrived coarse 4 s after
  the second client started watching, and the typed line came back in the stream.
