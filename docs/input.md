# Input

The machine has a keyboard and a pointer. Like the display they are plain RAM at fixed physical
addresses: the host writes them once per frame, in the GPU device's control pass, and a program
reads them with ordinary loads. The CPU shader has no input code. The console (UART) is
separate and unchanged.

| Address | Contents |
|---|---|
| `0x87000020` | pointer x, in display pixels |
| `0x87000024` | pointer y |
| `0x87000028` | buttons: bit 0 left, bit 1 right, bit 2 middle |
| `0x8700002c` | number of key events written so far |
| `0x87000030` | keyboard owner: a program that takes its keys from this device writes `0x6b657973` here, and 0 when it stops |
| `0x87000080` | ring of the last 32 key events, one word each |

- The pointer is where the host's pointer is over the picture, in the display's own pixels
  (0..width-1, 0..height-1, clamped), whatever size the picture is shown at. It needs a display
  mode to be set.
- A key event is a Linux key code (`KEY_A` = 30; the numbers of `linux/input-event-codes.h`)
  with bit 31 set for a press and clear for a release. The host does not repeat keys.
- Event number n is at ring position n mod 32. A reader keeps its own count: while it differs
  from the word at `0x8700002c`, read the next ring entry. If more than 32 are outstanding the
  reader has lost some and should restart from the current count.
- At most four key events arrive per frame; the host queues the rest.
- A notch of the pointer's wheel travels in the same ring, as a "press" of a code that is no
  key: `0x3fe` up, `0x3ff` down. It needs no release.
- The four state words share one RAM texel, so a reader sees one consistent set.
- A host that also feeds typed keys to the console (the harness window does) stops doing so
  while the keyboard has an owner. Otherwise a key typed into a terminal window would also
  be typed at the console's shell.

## From Linux

This project's kernel (`linux/kernel/shaderemu_input.c`) presents the block as two evdev
devices, polled every timer tick (100 Hz):

| Device | Events |
|---|---|
| `/dev/input/event0` "ShaderEmu keyboard" | `EV_KEY` for the key codes, with the kernel's auto-repeat |
| `/dev/input/event1` "ShaderEmu pointer" | `EV_ABS` `ABS_X`, `ABS_Y` in display pixels; `EV_KEY` `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE`; `EV_REL` `REL_WHEEL` |

Nano-X's pointer driver turns a notch into one of Microwindows' scroll buttons, down in one
report and up in the next; a program sees a button-down event with `GR_BUTTON_SCROLLUP` or
`GR_BUTTON_SCROLLDN` (`ui_wheel()` in `linux/apps/ui.h`).

## In the harness

The window's keys and pointer feed the device (`memoryViewTakeInput()` in
`harness/core/memview.cpp`): scan codes are translated to Linux key codes, and the pointer is
given in window pixels with the size of the display panel; the control pass maps it to display
pixels with the same scale and centring the view draws with. Keys typed into the window also
reach the console as characters, unless the keyboard has an owner; the bar under the display
says which. The harness reads the control words back with row 0 of the state every frame.

Under Linux the owner word is written by the program through `/dev/gpu` (Nano-X's screen
driver does), and the kernel clears it, and the cursor, when that file is closed, so a
program that dies does not keep the keyboard.

In a VRChat world the same uniforms (`_InputPointer`, `_InputButtons`, `_InputKeySeq`,
`_InputKeyCount`, `_InputKey0`..`_InputKey3` on the GPU device's material) are set by a script.

Input there is the world's own beams (`EmuPointer.cs`), one per hand in VR and the middle of
the view on desktop. VRChat's laser cannot serve: it works on one hand at a time, and a world
is told that a canvas was pressed, never where.

- A beam runs along the back of the hand, from the avatar's bones (wrist to knuckles, or elbow
  to wrist), and is drawn as a line with a dot where it lands. The control panel tips it up
  or down for controllers that point elsewhere.
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
