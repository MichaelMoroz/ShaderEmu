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
| `/dev/input/event1` "ShaderEmu pointer" | `EV_ABS` `ABS_X`, `ABS_Y` in display pixels; `EV_KEY` `BTN_LEFT`, `BTN_RIGHT`, `BTN_MIDDLE` |

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
