# Input

The machine has a keyboard and a pointer. Like the display they are plain RAM at fixed physical
addresses: the host writes them once per frame, in the GPU device's control pass, and a program
reads them with ordinary loads. The CPU shader has no input code. The console (`console.md`)
is separate.

| Address | Contents |
|---|---|
| `0x81000020` | pointer x, in display pixels |
| `0x81000024` | pointer y |
| `0x81000028` | buttons: bit 0 left, bit 1 right, bit 2 middle |
| `0x8100002c` | number of key events written so far |
| `0x81000030` | keyboard owner: a program that takes its keys from this device writes `0x6b657973` here, and 0 when it stops |
| `0x81000080` | ring of the last 32 key events, one word each |

- The pointer is where the host's pointer is over the picture, in the display's own pixels
  (0..width-1, 0..height-1, clamped), whatever size the picture is shown at. It needs a display
  mode to be set.
- A key event is a Linux key code (`KEY_A` = 30; the numbers of `linux/input-event-codes.h`)
  with bit 31 set for a press and clear for a release. The host does not repeat keys.
- Event number n is at ring position n mod 32. A reader keeps its own count: while it differs
  from the word at `0x8100002c`, read the next ring entry. If more than 32 are outstanding the
  reader has lost some and should restart from the current count.
- At most four key events arrive per frame; the host queues the rest.
- A notch of the pointer's wheel travels in the same ring, as a "press" of a code that is no
  key: `0x3fe` up, `0x3ff` down. It needs no release.
- The four state words share one RAM texel, so a reader sees one consistent set.
- A host that also feeds typed keys to the console (the harness window does) does so only
  while no display is up (control word 0, the display's mode, is 0) and the keyboard has no
  owner. Otherwise a key typed into a window would also be typed at the console's shell.

## A beam as the pointer, steadied (the VRChat world)

A hand in a headset shakes by a few pixels of the display, and a click with a beam was often a
small drag, or a double click two clicks apart (a unit missed in Command & Conquer, a move-to
that did not take). `EmuPointer.Steady` stands between the beam and the pointer words while the
control panel's "Steady pointer" is on (it is unless a player turns it off; kept in their
PlayerData as `pointer.steady`), in a headset only:

- The beam's pixel goes through a one-euro filter: its cut-off is `steadyCutoff` (1.2 Hz) plus
  `steadyBeta` (0.012) times its speed in pixels a second, so a still hand is held and a fast
  sweep follows. Simulated at 90 frames a second with a hand's shake of 2.5 pixels (rms), the
  pointer shook by 0.63; a sweep of 400 pixels in 0.4 s was 7 pixels behind at its end and
  where the beam was 0.1 s after.
- A press holds the pointer where it went down until the beam has moved `dragSlop` (7) pixels
  from there: a click stays a click, a drag starts after those pixels.
- A press within `doubleTime` (0.5 s) of the last release and within `doubleSlop` (10) pixels of
  the last press goes down on that press's place: the two clicks of a double click land on one
  pixel. The pointer stays there between them unless the beam moves away.

The guest's own thresholds (the window system's double click, a game's drag box) are unchanged.
Not yet tried in a headset.

## From Linux

This project's kernel (`linux/kernel/shaderemu_input.c`) presents the block as two evdev
devices, polled every timer tick (100 Hz; every fourth tick on an idle machine nobody has
touched for a second, `IDLE_POLL`). A key or a button is reported in its tick. A
pointer that only moves is reported 50 times a second while the machine is idle and 10 times
a second while a program is running: every report wakes the window system, and with a game
running that was 17 thousand instructions a report of a machine that has three million a
second, and not at all inside the window system's quiet rectangle (`pointer_hushed`;
`nanox.md`, "The pointer on a core of its own"). The cursor does not wait for it where the
display is drawn under the host's own pointer (below), and a program that wants every move
reads the words above itself:

| Device | Events |
|---|---|
| `/dev/input/event0` "ShaderEmu keyboard" | `EV_KEY` for the key codes, with the kernel's auto-repeat |
| `/dev/input/event1` "ShaderEmu pointer" | `EV_ABS` `ABS_X`, `ABS_Y` in display pixels; `EV_KEY` `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`; `EV_REL` `REL_WHEEL` |

Nano-X's keyboard driver (`kbd_shaderemu.c`) reports a key's release as the key its press
was: a letter let go while Ctrl or Shift had come down meanwhile was another key's release
(W under Ctrl is 0x17), which a game drops, and it went on walking.

Nano-X's pointer driver turns a notch into one of Microwindows' scroll buttons, down in one
report and up in the next; a program sees a button-down event with `GR_BUTTON_SCROLLUP` or
`GR_BUTTON_SCROLLDN` (`ui_wheel()` in `linux/apps/ui.h`).

## A program's own reading

A game takes its keys and buttons from the window system, which knows whose they are, and
the pointer's place from the words at `0x81000020`, which cost two loads (`pointer_read()` in
`linux/tdawn/host.c`, `Sys_SendKeyEvents` in `linux/quake/vid_shaderemu.c`,
`ReadPointer` in `linux/classicube/Window_ShaderEmu.c`). It asks the window system for events
only when the button word or the key count has changed, and a few times a second. It does
not select the pointer's moves: each was a message to read after a question to ask, two
system calls and two task switches, wherever the game looked for input. What the window
system is still asked for is the pointer coming over the window and leaving it (another
window may lie over this one), and where the window is (`GrGetWindowInfo` up to the root:
when the pointer comes in, at a change of the window, and every three seconds, since a
frame can be moved without a word to the window in it).

Measured on Red Alert's first Soviet mission with the pointer going round (`--pointer-sweep`
below): 10 to 11 frames a second with events for every move, 100 a second from the kernel;
17 as it is now; 19 to 20 with the pointer at rest. The whole run, from the command to frame
300, was 365 million instructions and is 304 million; it is 300 with the pointer at rest.

## A 3D game's view on a flat screen

The pointer is a place on the display, not a mouse that can be held: its travel turned the
view wherever it went and stopped at the window's edge. Doom, Quake, Quake III and ClassiCube
turn the view only while the left button is held on the game, as a finger drags a picture
(a quarter of a degree a pixel; the drag goes on outside the window). The right button
fires in the three shooters. In ClassiCube it places as before, and a left press let go where
it began, within 0.4 s, is the left click. A menu has both buttons as they were.
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
it (15 October 2026): every other program's close of `/dev/gpu` (a worker program, a game
refused the GPU, Files' pictures) cleared Nano-X's, and keys typed into windows reached the
console as well.

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
