# The classroom's computers

In the middle of the den are four tables in the main desk's oak with eight computers of the
decade, one a visitor (`classroom` in `world/furniture.py`; the computer is `world/pc.py`: a
monitor with a lofted housing and bulging glass, a tower with its drives and keys, a keyboard,
a mouse; the chairs are the main desk's). Each tube shows its owner's display, or their
console, to everybody. The tube is twice a 15 inch one's diagonal, so that a visitor without a
headset can read it, and its picture is on the glass itself: the glass's texture coordinates
are the picture's.

- A visitor's place is `station` in their own `EmuShare` (-1: none): a synced number only its
  owner can write, so nobody moves anybody else. `EmuStations` gives a new visitor the first
  free place; the key under a tube takes that place if it is free, and at a visitor's own
  turns the tube between display and console (`stationMode`, synced the same way). Two who name one place: the
  visitor with the lower player number has it, and the other looks for a free one.
- Whoever has a place sends their display as for a watcher (`docs/share.md`): the hub counts
  the room as one. `EmuStations.Received` puts a packet's tiles into that place's own store and
  decodes it into that place's picture; a visitor's own tube shows their machine itself.
- A client that is new to a place, or lost a packet, sets `askFrom` and counts `askCount` up
  in its own share; the place's owner sends everything again (at most every 3 s a place).
- A place's console is the rows the same packets carry, kept in a grid of its own and drawn by
  `Terminal.shader` into a 640 x 480 picture; a visitor's own is their terminal's.
- `CRT.shader`: the picture behind bulging glass, the beam's lines (one a row of the picture)
  and stripes of three phosphors. Lines and stripes are waves of mean one, each weakened by how
  much of its period a screen pixel spans: close up they show, further off they are their mean,
  and there is no moire between.

To test with one player: `ClientSimMain.SpawnRemotePlayer`, set the new `EmuShare`'s `station`,
copy this player's `flags`, `seq` and `packet` into it each time `seq` changes, and send
`EmuStations` the event `__0_Received` with `__1_share__param` set to it. The place's picture
must come to within a few levels of the machine's own display.
