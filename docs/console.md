# The console

The machine has one serial console (the UART; `/dev/hvc0` in Linux). In the VRChat world it
is four terminals, with the lines that scrolled off each, and a field to paste text into.

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
  (`0x8700003c`, `docs/gpu.md`). The world sets it; the harness does with `--tabs`, and
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
  (Ctrl+V on a desktop; VRChat's own keyboard in a headset). "Type it" sends it to the
  console's keyboard as if typed, a little at a time: the guest takes about 27 characters a
  second, so a page of text takes a while; "Clear" drops what is left.
- What other players see of the console (`docs/share.md`) is the terminal shown, at its end.
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
