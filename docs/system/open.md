# Open: a link or a file, with the program for its kind

A visitor of the VRChat world pastes an address into a field by the display and presses
Open; the machine fetches what the address holds and opens it in the window system with the
program registered for what it is. The file manager opens a file the same way.

    open ADDRESS-OR-FILE        open --dry FILE     (says the kind and the command, starts nothing)

## From the host to the guest

| Address | Contents |
|---|---|
| `0x81000240` | requests so far: the host adds 1 |
| `0x81000244` | the address's length, 255 at most |
| `0x81000250` | the address's first 176 bytes |
| `0x81000310` | and 80 more |

- The control pass writes them from the answer's texture with `_FetchDeliver` 4 (`HOST_OPEN` in
  `src/gpu.h`), in one frame, as it writes a fetch's answer.
- The desktop's bar looks once a second (`watch_open` in `linux/nanox/nxbar.c`) and starts
  `open` with the address. The last count it acted on is in `/tmp/nxopen.count`, so a bar
  started again does not open the same thing twice. One request at a time: a second one
  made before the bar has seen the first replaces it.
- The harness: `--open ADDRESS` (several, ten seconds apart), not before the console has shown
  the text of `--open-after TEXT`. An address may be a file of the host's (`file://C:\...`):
  the harness hands the guest only those of its files that an `--open` named. Its count goes
  on from the guest's own word, so an `--open` after a snapshot that had one is seen.
- The world: the field and the Open key over the display's keyboard (`EmuMachine.OpenLink`),
  and the strip at a classroom place (`OpenPlaceLink`, `docs/world/stations.md`). With no display
  mode set the bar is not running and nothing would take the address: the key says "start the
  desktop first".

## What `open` does (`linux/apps/open.c`)

1. A file in the machine is opened as it is; a folder in the file manager.
2. An address is asked of the host as a file (`docs/system/fetch.md`, kind 3) and written to
   `/tmp/open/NAME`, part by part when it is over 256 KB. A window says how far
   (`ui_progress.h`, from 0.7 s on): the seconds the host has been downloading it (VRChat
   gives a world the whole file or nothing, and says nothing meanwhile), then the megabytes
   handed over, a part of 256 KB at a time.
   A picture given as pixels is written a part at a time from a buffer, not a byte a `fputc`.
   A file that came inside a picture has its own name and a CRC-32, which is checked. A plain picture the host could only give
   as pixels becomes a PPM file.
   Closing that window (its frame's button, Alt+F4) ends `open` at once: the download is
   left to the host and nothing is opened.
3. When it does not come, a small window says why and goes at OK, Enter, Escape or by itself
   after 30 s (`tell`, a `ui_ask.h` question with one answer): "not found (404)", "refused
   (403)", "a picture the host could not decode (415)", "the host's download gave no answer
   (VRChat gives up after 20 s)" (status 2), "the host did not answer in 40 s" (no answer at
   all), "an address the host may not open" (3), "the world was not handed this address" (1,
   after a minute of asking again). A file that is not there or a program for another processor
   is said the same way. Without a window system it is only the console's line. How long each
   wait is: the host's answer 40 s, another program's request before this one 10 s, both
   by the clock.
4. Its kind: by its first bytes where a kind's marks say (after a byte order mark and blank
   lines too), else by its name's ending.
5. The kind's command is run with the file. A page that came from an address is given to
   the browser as that address, so its links and pictures are found.

## The kinds

Every file `/usr/share/nxopen.*`, a line a kind: its name, its endings, its first bytes, its
command. `-` is none, `\xNN` a byte, `\x..` any byte, `%s` the file. Endings are compared in either case. A program's build adds a file of its own,
as it adds its line to the Start menu (`nxapps.*`).

| File | Kinds |
|---|---|
| `nxopen.15-ide` (`linux/ide/build.sh`) | source: a `.c`, `.h`, `.js`, `.py` or `.hlsl` file is the IDE's (`docs/apps/ide.md`). Files are read in their names' order and the first kind that fits is the file's, so this comes before text's |
| `nxopen.20-apps` (`linux/apps/nxopen.conf`) | page, picture (PNG, JPEG, GIF, BMP, TGA, PNM, XPM), archive, program, text, shortcut (a desktop's `NAME.app`: `docs/system/nanox.md`) |
| `nxopen.50-doom` (`linux/nanox/doom.sh`) | a whole game's WAD (`doom -iwad`), a patch WAD (`doom -file`) |
| `nxopen.72-micropolis` (`linux/micropolis/build.sh`) | a city: `.cty` (`nxmicropolis`, `docs/games/micropolis.md`) |
| `nxopen.73-openttd` (`linux/openttd/build.sh`) | a saved game: `.sav` (`openttd -g`, `docs/games/openttd.md`) |
| `nxopen.83-pathtrace` (`linux/pathtrace/build.sh`) | a mesh: `.obj`, `.ply`; a scene: `.scene` (`nxpath`, `docs/apps/raytrace.md`) |
| `nxopen.84-play` (`linux/play/build.sh`) | sound: WAV, MP3, FLAC, Ogg Vorbis (`.ogg`, `.oga`), MOD (`docs/apps/play.md`) |
| `nxopen.85-nes` (`linux/nes/build.sh`) | a NES game |
| `nxopen.86-video` (`linux/video/build.sh`) | video: MPEG-1, VQA, MJPEG in AVI; a GIF is a picture's, animated or not (`docs/apps/video.md`) |

- **A program** (`\x7fELF`, `#!`): made executable and run in a terminal window of its own
  (`nxopen-run`), which stays until Enter when the program has ended. A program for another
  processor is said to be that and is not run.
- **An archive**: see "Archives" below.
- **Nobody's kind**: a small window that says so, with "Open as text" and "Save to /root".
- The file manager starts a file with its execute bit itself only when it also begins as a
  program does (`is_program` in `nxfiles.c`: every file of the image has that bit), and `open`
  for everything else; its own list of endings is only for the icons. `nxfiles --kinds FOLDER`
  prints each entry's kind and icon.

Nothing of this outlasts power-off: `/tmp` and `/root` are the overlay's.

## Archives

An archive is unpacked into `/tmp/open/NAME.d`, and the file manager opens on it
(`nxopen-archive`). `open --unpack` runs `unarc KIND FILE TO` first (`linux/apps/unarc.c`): a
zip's entries inflated a core each, a large one and a `.tar.gz` or `.gz` on every core (libmedia's
`media_inflate_stream`, `docs/system/media.md`, "One deflate stream on many cores"), its
`progress DONE TOTAL` lines shown as they come and the window looked at every 200 ms.

unarc exits 3 for what it does not unpack (an encrypted entry, a method other than stored and
deflate, zip64) and for a lack of memory in one piece (without an MMU it holds the archive and
what it makes whole), and `open` then runs the image's own `unzip` or `tar`, with a window that
says how far: a zip by the bytes in its folder against its listing's total, counted twice a
second, a tar, a `.tar.gz` or a `.gz` by the bytes it has been fed through a pipe, as the pipe has
room. A `.gz` of one file is unpacked and opened as what it holds. Closing the window stops the
unpacker, and nothing is opened.

## Checking it

- What it decides, in the interpreter: `open --dry` over one file of each kind and some with
  wrong endings or none (0.59 million instructions a file, `rvc_cpu`, 31 October 2026).
  `python tools/test_all.py open-kinds nxfiles-kinds nxview-unnamed` does it for every kind
  libmedia reads (`docs/system/media.md`), and the viewer given a picture under a name with no
  ending.

      bin\rvc_cpu.exe --quiet --uart-log logs\ucpu_open.log --expect "/ # " --send "open --dry /usr/share/doom1.wad; cp /usr/share/nes-thwaite.nes /tmp/game; open --dry /tmp/game; echo LX-''DONE\n" --until "LX-DONE"

- The whole way, in the harness: a file of 512 KB of noise that begins `NES\x1a`, wrapped by
  `tools\file_to_png.py`, is asked for in three parts and arrives with the host's own CRC-32.

      pwsh tools\gpu_queue.ps1 bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --no-stdin --desktop --uart-log logs\uart.log --open "file://C:\...\big.nes.png" --open-after "GPU drawing" --until "is nes"

- In the world's play mode: `openField.SetUrl(new VRCUrl(...))` and the machine's `OpenLink`
  event. The editor cannot download a test file, so an editor hook hands the machine the PNG
  as a texture when its `fetchState` becomes loading (`hostImage`, `fetchW`, `fetchH`,
  `fetchStatus` 200, `fetchState` 3): the control pass must read a file's bytes out of the sRGB
  texture exactly, and the file must arrive with the same sum.
- A closed window and a link that does not come, in the interpreter, which no host answers
  (status 0 after 40 s): `nano-X -p &`, then `open https://... &`, `nxkey alt+f4` 5 s later must
  say "open: stopped: the window was closed" at once. Left alone, with `UI_ASK_PRINT=1`: the
  window that says why is up after the 40 s ("could not be opened: the host did not answer in
  40 s") and goes at Enter. A `.gz` unpacked by `open --unpack` also ends at Alt+F4.
- A link that is no picture to VRChat's picture loader, which takes PNG and JPEG
  (`docs/system/fetch.md`), goes to its text loader; status 2 after 20 s is what comes back when
  VRChat gives nothing.
- Not tried: a real download in VRChat itself, which needs the picture on a public https
  host. VRChat's own texture may differ from the editor's (its colour space, a size limit).

The registry's scan keeps only the `nxopen.*` names and reads their lines without `sscanf`.

"Open as text" starts the editor as a program of its own: started in `open`'s place, which has
a window of its own, the editor's window would get no frame.
