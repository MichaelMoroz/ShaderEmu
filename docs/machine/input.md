# Input

The machine has a keyboard and a pointer. Like the display they are plain RAM at fixed physical
addresses: the host writes them once per frame, in the GPU device's control pass, and a program
reads them with ordinary loads. The CPU shader has no input code. The console (`console.md`)
is separate.

| Address | Contents |
|---|---|
| `0x81000020` | pointer x, in display pixels |
| `0x81000024` | pointer y |
| `0x81000028` | buttons: bit 0 left, bit 1 right, bit 2 middle; 3 and up below ("More of a mouse and the hands") |
| `0x8100002c` | number of key events written so far |
| `0x81000030` | keyboard owner: a program that takes its keys from this device writes `0x6b657973` here, and 0 when it stops |
| `0x81000080` | ring of the last 32 key events, one word each |

- The pointer is where the host's pointer is over the picture, in the display's own pixels
  (0..width-1, 0..height-1, clamped), whatever size the picture is shown at. It needs a display
  mode to be set. The host gives it as a place over a panel (`_InputPointer`: x, y and the panel's size), and
  `gpu.h` scales the picture into the panel with no margin: the world gives the display's own pixels over a panel
  of the display's size, the place its `Display.shader` draws the cursor at (`_HostPointer`). Until 7 November 2026
  the world still added the margin `gpu.h` lost on 17 October (x + 8 over a panel 16 larger): the guest's pointer
  was up to 8 px toward the picture's middle from the drawn cursor, and a still pointer on a caption's close box lit
  and pressed the box beside it. `rvc_cpu --pointer-at S,X,Y` holds the pointer still (`test_all.py wm-caption-still`).
- A key event is a Linux key code (`KEY_A` = 30; the numbers of `linux/input-event-codes.h`)
  with bit 31 set for a press and clear for a release. The host does not repeat keys.
- Event number n is at ring position n mod 32. A reader keeps its own count: while it differs
  from the word at `0x8100002c`, read the next ring entry. If more than 32 are outstanding the
  reader has lost some and should restart from the current count.
- At most four key events arrive per frame; the host queues the rest.
- A notch of the pointer's wheel travels in the same ring, as a "press" of a code that is no
  key: `0x3fe` up, `0x3ff` down, `0x3fc` left, `0x3fd` right (the wheel tilted). It needs no
  release. Bits 12-13 say whose wheel it is: 0 the pointer's own (a mouse's), 1 the left
  hand's stick, 2 the right hand's (below).
- The four state words share one RAM texel, so a reader sees one consistent set.
- A host that also feeds typed keys to the console (the harness window does) does so only
  while no display is up (control word 0, the display's mode, is 0) and the keyboard has no
  owner. Otherwise a key typed into a window would also be typed at the console's shell.

## More of a mouse and the hands

A game can use more than three buttons and a wheel: a mouse's side buttons, a wheel tilted, and in
a headset two hands, each with a stick, a trigger, a grip and buttons. They come in the same words,
the same evdev device and the same window system, without a new request or a change to Nano-X's
client library (3 November 2026; `linux/apps/input_words.h` names them for C).

The buttons word at `0x81000028`:

| Bit | |
|---|---|
| 0, 1, 2 | the pointer's left, right, middle (a trigger or a click; a grip or the right click) |
| 3, 4 | a mouse's side buttons: back, forward |
| 7 | the bits below are told (a headset) |
| 8, 9 | the left hand's trigger, grip |
| 10, 11 | the right hand's trigger, grip |
| 12, 13 | the left stick pressed in, the right one |
| 14, 15 | the upper face button of the left hand (Y), of the right (B) |
| 16 | the jump button (A) |

The hands' bits are whichever hand has the pointer or not: a game tells the hands apart by them,
while bits 0 and 1 stay the pointer's. The world tells them while a beam is on the display or the
sitter holds the controller (`EmuPointer.MoreButtons`, `EmuMachine.moreButtons`), and 0 otherwise.

The wheels, in the key ring (above): the pointer's own (a mouse's, `0x3fc` to `0x3ff`), and in a
headset the right hand's stick while its beam is on the display, at up to 12 notches a second
in each axis: it was always the wheel, and is now across too, and the player stands still
(`Immobilize`) while it scrolls. The left stick always walks (6 November 2026): as a second wheel
while its beam was on the display it held a player before the screen in place, both beams being
on it there; its axes below are never turned from the world. On a desktop, the mouse's wheel with Shift held is the wheel
tilted (Unity has no axis for a tilt), and the side buttons are bits 3 and 4.

**From Linux** (`linux/kernel/shaderemu_input.c`), on `/dev/input/event1`:

| What | evdev |
|---|---|
| the pointer's wheel and the right hand's, up | `REL_WHEEL` |
| the same, across | `REL_HWHEEL` |
| the left hand's stick, up | `REL_DIAL` |
| and across | `REL_MISC` |
| bits 3, 4 | `BTN_SIDE`, `BTN_EXTRA` |
| bits 8 to 16 | `BTN_TRIGGER_HAPPY1` to `BTN_TRIGGER_HAPPY9` (`0x2c0` + bit - 8) |

**In Nano-X** a window asks for them with `GrSetWMProperties(window, flags GR_WM_FLAGS_COMMAND,
props GR_WMC_BUTTONS, background 1)`; then it and the windows in it get button events of their own
for `GR_BUTTON_SCROLLLEFT`, `_SCROLLRIGHT` (the wheel tilted, the right stick across), `GR_BUTTON_BACK`,
`_FORWARD`, and `GR_BUTTON_SCROLL2UP`, `_2DN`, `_2LEFT`, `_2RIGHT` (the second wheel: the left hand's
stick), down in one report and up in the next as the wheel's are (`nano-X.h`, `GR_BUTTON_EXTRA`). They
go to the window under the pointer (or the one that has it grabbed) and up to the window that asked.
A window that did not ask is told nothing of them: most programs take any button that goes down for a
click. The usual buttons' events are as they were, their buttons word without these. The hands' own
buttons are no Nano-X event: a program reads the word, which costs a load.

**To test without the world**: `rvc_cpu` and `rvc_harness` take `--scroll S,W,N` (at second S, N notches
of wheel W: `y`, `x` the pointer's, `ly`, `lx` the left hand's, `ry`, `rx` the right hand's),
`--input-event S,HEX` (any ring event), and `--pointer-button B,S,T` in hex (`0x419`: left, back,
forward and the right hand's trigger). The harness's window gives the side buttons and a tilted wheel
(`WM_XBUTTON*`, `WM_MOUSEHWHEEL`). `nxinput [SECONDS [plain]]` prints what each of the three ways
gives, and `test_all.py input-more`, `input-plain` hold them. What only Unity can show: the hands' bits
(`Oculus_CrossPlatform_*` axes read with `Input.GetAxis`), the sticks as wheels and `Immobilize`
stopping VRChat's own turn; none was run there (compiled against the world's assemblies).

## Game controls

What each game does with them (3 November 2026). A desktop game asks for the extra buttons and reads
the hands' buttons from the word where its loop wakes, through `linux/apps/ui_pad.h` (`ui_pad_ask`,
`ui_pad_poll`, `ui_pad_scroll`, `ui_pad_arrow`, `ui_pad_key`); while a headset's hands are told
(bit 7) it wakes at least ten times a second for them, since they are no event. A beam on the
display is still the pointer: the trigger is the left button and the grip the right one, as before.
"Left stick" and "right stick" below are each hand's stick with that hand's beam on the display, or
a mouse's wheel tilted (Shift) and turned on a desktop.

| Game | Left stick (second wheel) | Right stick (wheel) | Pointer's buttons | A / B / Y |
|---|---|---|---|---|
| Tetris (`nxtetris`) | across moves a cell a notch, down a row, up holds | turns (up or right clockwise) | a click on the well turns, the right button drops | A drops (or starts), B holds, Y pauses |
| Pinball (`nxpinball`) | | | with a mouse: left and right buttons on the table are the flippers, middle or a side button the plunger | each hand's trigger is its flipper, a grip or A the plunger, Y pauses |
| Mines (`nxmine`) | moves the keyboard's cell | | as before: open, flag | A opens the cell, B flags it |
| Solitaire (`nxsolitaire`) | | | as before: drag, click | A turns the stock, B takes back, Y finishes |
| Chess (`nxchess`) | moves the keyboard's square | | as before | A picks and drops, B takes back |
| Micropolis (`nxmicropolis`) | pans the map 64 pixels a notch (over the map) | scrolls the map up and down (its wheel) | as before | A pauses and goes on |
| Tiberian Dawn, Red Alert (`linux/tdawn/host.c`) | pans the map: an arrow key pressed and let go a notch | | as before (the right button dragged scrolls) | Y is Escape |
| OpenTTD (`shaderemu_v.cpp`) | pans the main view: an arrow held 120 ms a notch | zooms (its wheel) | as before | |
| Age of Empires (`port/window.c`) | pans the map: an arrow held 120 ms a notch | | as before | |
| Quake, Quake III, Half-Life, Jedi Outcast | | the next weapon up or right, the one before down or left (Half-Life: its own wheel binds) | as before | A jumps, B uses (not Quake), the left stick pressed crouches (not Quake), Y is Escape |
| Doom | | | as before | A or B uses |
| ClassiCube, not seated (`Window_ShaderEmu.c`) | walks: a notch holds W, S, A or D 0.2 s | as before (the held block) | as before | A is Space (jump), B the inventory, Y is Escape |
| NES (osd_shaderemu.c) | the d-pad: a notch holds its way 9 frames (0.15 s) | | | A and B the pad's, Y Select, the right stick pressed Start |

A sitter in a holodeck's seat keeps the controller's own mapping (`EmuGamepad`): the 3D games take none of
these while seated. Groups of units in the C&C games and AoE are not mapped: the game says nothing of which
groups exist, so a button that cycles them would select empty ones.

`NXTETRIS_PRINT=1` and `NXPINBALL_PRINT=1` say each move a controller made, and `test_all.py
pad-tetris`, `pad-pinball`, `pad-mines`, `pad-chess`, `pad-tdawn`, `pad-openttd`, `pad-quake` and `pad-nes` play them with `--scroll` and `--pointer-button`.

## A beam as the pointer, steadied (the VRChat world)

A hand in a headset shakes by a few pixels of the display, which makes a click with a beam a
small drag, or a double click two clicks apart. `EmuPointer.Steady` stands between the beam and the
pointer words while the control panel's "Steady pointer" is on (it is unless a player turns it off;
kept in their PlayerData as `pointer.steady`), in a headset only:

- The beam's pixel goes through a one-euro filter: its cut-off is `steadyCutoff` (1.2 Hz) plus
  `steadyBeta` (0.012) times its speed in pixels a second, so a still hand is held and a fast
  sweep follows.
- A press holds the pointer where it went down until the beam has moved `dragSlop` (7) pixels
  from there: a click stays a click, a drag starts after those pixels.
- A press within `doubleTime` (0.5 s) of the last release and within `doubleSlop` (10) pixels of
  the last press goes down on that press's place: the two clicks of a double click land on one
  pixel. The pointer stays there between them unless the beam moves away.

The guest's own thresholds (the window system's double click, a game's drag box) are unchanged.
Not yet tried in a headset.

## The screen's edge (the VRChat world)

A mouse rests on the screen's edge; a beam has nothing to rest on, and the sample after the last on
the picture used to be off it, the pointer gone. So the room's display and a holodeck's screen have a
margin round the picture, `EmuPointer.edgeMargin` (0.04: 4% of the picture's width at its sides, 4% of
its height above and below), where the beam is still the pointer, held on the nearest pixel of the
edge, buttons as they are (a press there is a press on the edge). Edge scrolling, a button in a corner
and a drag that rests on the edge work with a hand that wavers by a few centimetres. The margin is the
display's plane, not its collider: a beam past the quad's side misses the collider, so `OnPlane` meets
the plane itself, and anything of the room 10 cm or more before it (not the display's own frame)
stops the beam as before. Beyond the margin a button pressed on the picture still holds the pointer
on the edge until it is let go (`EdgeOf`, "A drag that leaves the screen" in `docs/system/nanox.md`).
On the edge the steady pointer does not smooth: a lag there would stop short of it. The desktop's
mouse is the middle of the view, the same beam, and has the margin too. A classroom tube has none (its
glass is curved).

`EdgeOf` had a fault of its own (3 November 2026): it took the beam's way into the quad's frame
unscaled and its origin scaled, so on the room's display (a unit quad scaled to 16:9) the place along
the edge where a beam ran off was wrong by the quad's shape. Both now meet the plane in the world.
Compiled against the world's assemblies; not run in Unity or a headset.

## From Linux

This project's kernel (`linux/kernel/shaderemu_input.c`) presents the block as two evdev
devices, polled every timer tick (100 Hz; every fourth tick on an idle machine nobody has
touched for a second, `IDLE_POLL`). A key or a button is reported in its tick. A
pointer that only moves is reported 50 times a second while the machine is idle and 10 times
a second while a program is running, since every report wakes the window system, and not at all
inside the window system's quiet rectangle (`pointer_hushed`; `nanox.md`, "The pointer on a core of
its own"). The cursor does not wait for it where the display is drawn under the host's own pointer
(below), and a program that wants every move reads the words above itself:

| Device | Events |
|---|---|
| `/dev/input/event0` "ShaderEmu keyboard" | `EV_KEY` for the key codes, with the kernel's auto-repeat |
| `/dev/input/event1` "ShaderEmu pointer" | `EV_ABS` `ABS_X`, `ABS_Y` in display pixels; `EV_KEY` `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`; `EV_REL` `REL_WHEEL`; and those of "More of a mouse and the hands" |

Nano-X's keyboard driver (`kbd_shaderemu.c`) reports a key's release as the key its press
was, whatever Ctrl or Shift have done meanwhile: another key's release is one a game drops.

Nano-X's pointer driver turns a notch into one of Microwindows' scroll buttons, down in one
report and up in the next; a program sees a button-down event with `GR_BUTTON_SCROLLUP` or
`GR_BUTTON_SCROLLDN` (`ui_wheel()` in `linux/apps/ui.h`).

## A program's own reading

A game takes its keys and buttons from the window system, which knows whose they are, and
the pointer's place from the words at `0x81000020`, which cost two loads (`pointer_read()` in
`linux/tdawn/host.c`, `Sys_SendKeyEvents` in `linux/quake/vid_shaderemu.c`,
`ReadPointer` in `linux/classicube/Window_ShaderEmu.c`). It asks the window system for events
only when the button word or the key count has changed, and a few times a second. It does
not select the pointer's moves: each is a message to read after a question to ask, two
system calls and two task switches, wherever the game looks for input. What the window
system is still asked for is the pointer coming over the window and leaving it (another
window may lie over this one), and where the window is (`GrGetWindowInfo` up to the root:
when the pointer comes in, at a change of the window, and every three seconds, since a
frame can be moved without a word to the window in it).

## A 3D game's view on a flat screen

The pointer is a place on the display, not a mouse that can be held. Doom, Quake, Quake III,
Half-Life and ClassiCube turn the view only while the left button is held on the game, as a finger drags a picture
(a quarter of a degree a pixel; the drag goes on outside the window). The right button
fires in the shooters. In ClassiCube it places, and a left press let go where
it began, within 0.4 s, is the left click. In a menu both buttons are what they are.
`rvc_cpu --pointer-sweep 0 --pointer-circle X,Y,R --pointer-button 1,S,T` drags it round.

## In the harness

The window's keys and pointer feed the device (`memoryViewTakeInput()` in
`harness/core/memview.cpp`): scan codes are translated to Linux key codes, and the pointer is
given in window pixels with the size of the display panel; the control pass maps it to display
pixels with the same scale and centring the view draws with. Keys typed into the window
reach the console as characters only while nothing is on the display (the text console's
boot) and the keyboard has no owner; the bar under the display says which. The window takes
every key and system key itself, Alt+F4 apart, so a held key repeats in the guest only, and
losing the focus lets go of every key held. The harness reads the control words back with row 0 of the state every frame.

`--pointer-sweep N` moves the pointer round the middle of the display from frame N on,
without a window: what a program pays for a pointer that moves is otherwise only seen by
hand.

`--record-input FILE` writes the window's input with its wall seconds, a line an event: `S P x y w h buttons`
when the pointer changes, `S K code` a key event (hex), `S T bytes` what was typed (hex). `--replay-input
FILE` plays such a file instead of the window's input (Ctrl+], the harness's own quit, still quits). A file written by hand works too: `0.5 T` with an `echo`'s bytes types it into a shell snapshot.

Under Linux the owner word is written by the program through `/dev/gpu` (Nano-X's screen
driver does), and the kernel clears it, and the cursor, when that file is closed, so a
program that dies does not keep the keyboard. Only a file opened while the word was 0 clears
it: another program's close of `/dev/gpu` (a worker program, Files' pictures) leaves Nano-X's.

In a VRChat world the same uniforms (`_InputPointer`, `_InputButtons`, `_InputKeySeq`,
`_InputKeyCount` on the machine's material, and the keys as `_InputKeyCode0`..`_InputKeyCode3`,
a code plus 65536 while pressed, which `Machine.shader` turns into `_InputKey0`..`_InputKey3`)
are set by a script (`EmuMachine.cs`).

Input there is the world's own beams (`EmuPointer.cs`), one per hand in VR and the middle of
the view on desktop. VRChat's laser cannot serve: it works on one hand at a time, and a world
is told that a canvas was pressed, never where.

- A beam runs the way VRChat's laser went when it clicked the control panel's "Calibrate
  laser" dot (`Calibrate`, kept in PlayerData as `beam.left` and `beam.right`), else along the
  back of the hand from the avatar's bones (wrist to knuckles, or elbow to wrist), else along
  the controller. It is drawn as a line with a dot where it lands.
- A beam is faint while it only points and solid while its trigger or grip is held, and is
  not drawn at all when it points at neither the display nor a keyboard.
- On the display it is the pointer: trigger or click is the left button, grip or the right
  mouse button the right one. A hand holding a button keeps the pointer. The mouse's wheel,
  or the right stick pushed up and down in VR, is the wheel.
- The display draws the guest's cursor under the beam itself (`_HostPointer` in
  `Display.shader`, with the hot spot the guest publishes): the guest's own idea of where the
  pointer is arrives a few frames late.
- A beam that meets the open links panel (`fetch.md`) is not the pointer: the panel stands
  in front of the display, and a click on one of its fields would click the guest too.
- On a keyboard (`EmuKeyboard.cs`: 104 keys, found from their rectangles) the key under the
  beam is down from the trigger's press to its release, one key a hand. Shift, Ctrl and Alt
  also latch for the next key when pressed and let go alone.
