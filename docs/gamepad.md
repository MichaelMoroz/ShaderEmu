# The game controllers

The display's keyboard and the beams are a poor way to play: a shooter wants two sticks, and a
strategy game a pointer that does not have to be aimed with a whole arm. In the VRChat world
two controllers lie on the desk, to the right of the display's keyboard, a white one for
shooters and a blue one for strategy games. A visitor who takes one plays with their own
sticks, triggers and grips (`EmuGamepad.cs`, one behaviour a controller).

Nothing in the guest knows of them: a controller presses the keys and moves the pointer the
games already read (`docs/input.md`). There is no menu; what the controls do is on the board
on the left wall (`DoText` in `ShaderEmuGamepad.cs`), and each controller's name lies flat on
the desk before it.

## Taking one and putting it back

- Click a controller. Its model goes to the right hand (before the chest on a desktop), the
  beams rest, and the visitor stands where they are: a station holds them, so that the
  sticks are the game's and not their legs', and they are told to stand still besides.
- Both grips held for a second puts it back on the desk. Held for a second again, from
  anywhere in the room, they bring the one held last back into the hand (the shooter's, to
  begin with). After either, both grips have to be let go before they count again.
- On a desktop, which has no grips, the drop key (G) puts it back.
- Taking one while holding the other puts the other back. A respawn puts back whichever is held.
- Only whoever took it has it; nothing of it is sent to other players.

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

In play mode: `SendCustomEvent("Take")` on "Shooter controller" or "Strategy controller", then
`RunInputEvent("_inputMoveVertical", new UdonInputEventArgs(0.9f, HandType.LEFT))` and the
like, and read the display keyboard's `queue` (a key code, plus 65536 while pressed) and the
machine's `pointerX`. The editor is a desktop: the grips are tested by setting each
behaviour's `grip` to two trues for a second. ClientSim logs "Cannot exit station that the
player is not in" when one is put back, and the editor pauses on it if told to pause on errors.

"ShaderEmu/Add the game controllers to the open scene" puts them, and the board's text, into
a scene without building the world again.

## Not known yet

- None of it has been in a headset. Whether a station stops the right stick from turning
  the visitor in VR is what the station is there for and is untried (in the editor the
  station does not seem to take the player at all); if it does not, the view turns with the game.
- How the model sits in a hand: it is placed at the tracked hand with one fixed turn.
- Doom has no key for looking up, and neither game a second stick's analogue turn.
