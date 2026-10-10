# The game controllers

The display's keyboard and the beams are a poor way to play: a shooter wants two sticks, and a
strategy game a pointer that does not have to be aimed with a whole arm. In the VRChat world
the console beside every holodeck's seat (`docs/holodeck.md`) has a key for each of two
controllers, a white one for shooters and a blue one for strategy games; none lies there.
Whoever sits there and takes one plays with their own sticks, triggers and grips
(`EmuGamepad.cs`, one behaviour a client).

Nothing in the guest knows of them: a controller presses the keys and moves the pointer the
games already read (`docs/input.md`). What the controls do is on the board on the den's left
wall (`DoText` in `ShaderEmuGamepad.cs`).

There are none at the den's desk or in the classroom, and no seats there: a controller that
anybody could take anywhere held its player in an unsynced station, which others could not
see and which went wrong when two took it.

## Taking one and putting it back

- Only from the seat: the stand's Shooter and Strategy keys are the sitter's. The model goes to
  the right hand (before the chest on a desktop), the beams
  rest, and the seat keeps its sitter (`disableStationExit`), so that the left stick is the
  game's and does not stand them up.
- Everybody sees it: which one a visitor holds is a synced number in their own `EmuShare`
  (`holding`), and each player's object carries the two models, which `EmuHolodeck` puts in
  that player's right hand on every client.
- A notice hangs over the holder's hand, for them alone, for ten seconds after taking it and
  whenever a grip is held: what it is, and that both grips held for a second put it back. A
  bar fills while they are held.
- The stand's Put back key does the same, and is the way on a desktop, which has no grips.
- Taking one while holding the other puts the other back. Getting up, a respawn and leaving
  put back whichever is held.

## The white one: shooters (Quake, Doom, by their own default keys)

| Control | Key | In the games |
|---|---|---|
| left stick, up and down | Up, Down | walk |
| left stick, sideways | `,` `.` | step aside |
| right stick, sideways | Left, Right | turn |
| right stick, up and down | `a` `z` | look up and down (Quake) |
| right trigger | Ctrl | fire |
| left trigger | Space | jump (Quake), open (Doom) |
| right grip | `/` | next weapon (Quake) |
| left grip | Esc | menu |
| jump button | Enter | accept |

A stick pushed a little holds its turn key for part of every tenth of a second, more the
further it is pushed: a slow turn from keys that are only up or down. Quake's own mouse look
is not used: the guest's pointer is a place on the display, not a movement, and would stop at
the display's edge.

## The blue one: strategy (Red Alert, Command & Conquer)

| Control | Does |
|---|---|
| right stick | moves the pointer, faster the further it is pushed (`pointerSpeed`, 700 pixels a second at the end) |
| right trigger, left trigger | the left and the right button |
| left stick | the arrow keys, by which both games scroll their maps |
| right grip held, right stick up and down | the wheel |
| left grip, jump button | Esc, Enter |

On a desktop the same events come from the keyboard and mouse VRChat walks with: W A S D,
the mouse's movement, its two buttons, Space.

## Checking it

In play mode: `stations[k].UseStation(Networking.LocalPlayer)` on "Holodeck", then its
`TakeShooter` or `TakeStrategy` event, then `RunInputEvent("_inputMoveVertical", new
UdonInputEventArgs(0.9f, HandType.LEFT))` and the like on "Gamepad", and read the display
keyboard's `queue` (a key code, plus 65536 while pressed) and the machine's `pointerX`. `held`
on "Gamepad" and `holding` in the player's `EmuShare` must agree, the seat's
`disableStationExit` be set, and after `ExitStation` all of it be undone.

## Not known yet

- None of it has been in a headset. Whether a seat stops the right stick from turning the
  visitor in VR is untried; if it does not, the view turns with the game.
- How the model sits in a hand: it is placed at the tracked hand with one fixed turn.
- Doom has no key for looking up, and neither game a second stick's analogue turn.
