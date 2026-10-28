# The console

The machine has one console (`/dev/hvc0` in Linux): words in memory on the machine without an
MMU, which the world runs, and the UART on the one with it. In the VRChat world it
is four terminals, with the lines that scrolled off each, and a field to paste text into.

## Words in memory

On the machine without an MMU (`NO_PAGING` with `SBI_HLE`, `CSR_FIELDS` in `types.h`) the
console is memory and nothing else. The kernel reads and writes it with loads and stores, and
the shader has no part in it. It lies in the network's row, between the window and the ring
(`docs/system/lan.md`):

| RAM | |
|---|---|
| `0x876b8280` | characters the host has delivered so far (the host's) |
| `0x876b8284` | the last delivery, up to four characters, the first in the low byte (the host's) |
| `0x876b8290` | characters the guest has taken so far |
| `0x876b8294` | bytes the guest has written so far |
| `0x876b82a0` | the output ring: 256 bytes, byte n of the output at n mod 256 |

- The control pass writes the host's texel every frame (`CON_HOST` in `gpu.h`; the harness's
  `kConZone`). The host delivers again only once the guest has taken everything.
- Every host reads the ring back with the network's row (`kConHost` in `rvc_backend.h`,
  `ConTexel` in `EmuMachine.cs`): `rvc_harness` once a frame of all its rounds, the world once a
  round, `rvc_cpu` in its control pass, every pass.
- The guest puts at most 254 bytes while the host's clock word (`0x87000034`, `GPU_CLOCK`) is as
  it was: every host changes it only between two reads of the ring. It puts and tells one byte at
  a time after looking at the clock, so a pass that ends inside a write miscounts one byte at
  most. A full ring ends the pass with the pause hint. (The processor's time CSR was the measure
  until 28 October 2026: it moves every round, and `rvc_harness --tick-rounds` read a frame's
  rounds of 255 bytes each once; and a write that ran over a pass's end told its bytes in the next
  pass while it counted them in the last, so 510 bytes came between two reads even at one round a
  frame: 344 of 1,500 numbers lost.)
- The kernel's side is `linux/nommu/patch_console.py`: the hvc driver's `put_chars` and
  `get_chars`, and earlycon's `putc`. The SBI console calls are not answered.

The machine with the MMU keeps the UART: its receive register and the buffer of 64 characters
in the CPU's state, which the SBI console calls use.

## Four terminals on one line

Linux gives the machine no second console to open, so the terminals are made in the guest:
`emumux` (`linux/userland/emumux.c`) runs a shell on a pseudo-terminal for each and carries
them all over the console.

- Both ways, a byte `0x1e` and a digit `0` to `3` say which terminal the bytes after it
  belong to; `0x1e` twice is that byte itself. The guest says it when the source of its
  output changes; the host says it when the visitor picks a terminal.
- A terminal's shell starts when the terminal is first picked, and again when it ends.
  Each is 80 x 30 to the programs in it, as the world's screen is.
- `emuinit` starts `emumux` in the shell's place when the host asks: bit 1 of the host flags
  (`0x8700003c`, `docs/machine/gpu.md`). The world sets it; the harness does with `--tabs`, and
  without it the console is the one shell it was (snapshots and tests are as they were).
- What the kernel prints goes to the console itself, untagged: it lands in whichever
  terminal spoke last.

To check it in the harness (a cold boot, 5 s):

    bin\rvc_harness.exe --rvc experiments\rvc_opt --image linux-net --tabs --no-stdin ^
        --input "echo one-''tab\n" --expect "one-tab" --send "<0x1e>2tty\n" --until "/dev/pts/1"

## In the world

- `EmuTerminal` keeps a grid for each terminal: ten screens of lines, a ring. The buttons
  above the screen pick the terminal shown (one that printed while another was shown is
  lit) and scroll the view back and forth by half a screen; "Latest" returns to the end.
  A view scrolled back stays on its lines while more arrive.
- Picking a terminal types `0x1e` and its digit on the console's keyboard, so what is typed
  after it goes there.
- The field above the console's keyboard (`EmuPaste`) takes text typed or pasted into it
  (Ctrl+V on a desktop; VRChat's own keyboard in a headset). "Type as Text" sends it to the
  console's keyboard as if typed, a little at a time, and "+Enter" with Return after it: the
  guest takes about 27 characters a second, so a page of text takes a while. "Open Link" has
  the machine open the field's text as an address (`docs/system/open.md`).
- What other players see of the console (`docs/world/share.md`) is the terminal shown, at its end.
- "ShaderEmu/Add the console's tabs and paste field to the open scene" puts both into a
  scene without building the world again (run it twice the first time: the new behaviour's
  program has to exist before it can be filled in).

To test in play mode: `SendCustomEvent("Tab2")` on "Terminal screen", set the field's text
on "Console paste" and send `SendLine`, then read the terminal's `grid` (row `r` of terminal
`t` is at `(t * kept + (r + top[t]) % kept) * cols`).

## Not done

- The harness's own terminal mode shows the tagged stream as it is: with `--tabs` the tags
  are bytes on the screen. Tabs there would be a change to its console code.
- Scrolling by the wheel or by keys; a terminal's size other than 80 x 30.
- The kernel's messages in a terminal of their own.
