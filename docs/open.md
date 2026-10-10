# Open: a link or a file, with the program for its kind

A visitor of the VRChat world pastes an address into a field by the display and presses
Open; the machine fetches what the address holds and opens it in the window system with the
program registered for what it is. The file manager opens a file the same way.

    open ADDRESS-OR-FILE        open --dry FILE     (says the kind and the command, starts nothing)

## From the host to the guest

| Address | Contents |
|---|---|
| `0x87000240` | requests so far: the host adds 1 |
| `0x87000244` | the address's length, 255 at most |
| `0x87000250` | the address's first 176 bytes |
| `0x87000310` | and 80 more |

- The control pass writes them from the answer's texture with `_FetchDeliver` 4 (`HOST_OPEN` in
  `src/gpu.h`), in one frame, as it writes a fetch's answer.
- The desktop's bar looks once a second (`watch_open` in `linux/nanox/nxbar.c`) and starts
  `open` with the address. The last count it acted on is in `/tmp/nxopen.count`, so a bar
  started again does not open the same thing twice. One request at a time: a second one
  made before the bar has seen the first replaces it.
- The harness: `--open ADDRESS` (several, ten seconds apart), not before the console has shown
  the text of `--open-after TEXT`. An address may be a file of the host's (`file://C:\...`).
- The world: the field and the Open key over the display's keyboard (`EmuMachine.OpenLink`).

## What `open` does (`linux/apps/open.c`)

1. A file in the machine is opened as it is; a folder in the file manager.
2. An address is asked of the host as a file (`docs/fetch.md`, kind 3) and written to
   `/tmp/open/NAME`, part by part when it is over 256 KB. A file that came inside a picture
   has its own name and a CRC-32, which is checked. A plain picture the host could only give
   as pixels becomes a PPM file.
3. Its kind: by its first bytes where a kind's marks say (after a byte order mark and blank
   lines too), else by its name's ending.
4. The kind's command is run with the file. A page that came from an address is given to
   the browser as that address, so its links and pictures are found.

## The kinds

Every file `/usr/share/nxopen.*`, a line a kind: its name, its endings, its first bytes, its
command. `-` is none, `\xNN` a byte, `%s` the file. A program's build adds a file of its own,
as it adds its line to the Start menu (`nxapps.*`).

| File | Kinds |
|---|---|
| `nxopen.20-apps` (`linux/apps/nxopen.conf`) | page, picture, archive, program, text |
| `nxopen.50-doom` (`linux/nanox/doom.sh`) | a whole game's WAD (`doom -iwad`), a patch WAD (`doom -file`) |
| `nxopen.85-nes` (`linux/nes/build.sh`) | a NES game |
| `nxopen.84-play` (`linux/play/build.sh`) | sound: WAV, MP3, FLAC, MOD (`docs/play.md`) |

- **A program** (`\x7fELF`, `#!`): made executable and run in a terminal window of its own
  (`nxopen-run`), which stays until Enter when the program has ended. A program for another
  processor is said to be that and is not run.
- **An archive**: unpacked into `/tmp/open/NAME.d` by the image's own `unzip` and `tar`, and
  the file manager opens on it (`nxopen-archive`). A `.gz` of one file is unpacked and opened
  as what it holds.
- **Nobody's kind**: a small window that says so, with "Open as text" and "Save to /root".
- The file manager starts a file with its execute bit itself, as before, and `open` for
  everything else; its own list of endings is only for the icons.

Nothing of this outlasts power-off: `/tmp` and `/root` are the overlay's.

## Checking it

- What it decides, in the interpreter (0.7 s): `open --dry` over one file of each kind and
  some with wrong endings or none.

      bin\rvc_cpu.exe --quiet --uart-log logs\ucpu_open.log --expect "/ # " --send "open --dry /usr/share/doom1.wad; cp /usr/share/nes-thwaite.nes /tmp/game; open --dry /tmp/game; echo LX-''DONE\n" --until "LX-DONE"

- The whole way, in the harness (14 s from power-on): a file of 512 KB of noise that begins
  `NES\x1a`, wrapped by `tools\file_to_png.py`, is asked for in three parts and arrives with
  the host's own CRC-32.

      bin\rvc_harness.exe --d3d11 --rvc experiments\rvc_opt --image linux-net --no-stdin --desktop --uart-log logs\uart.log --open "file://C:\...\big.nes.png" --open-after "GPU drawing" --until "is nes"

- In the world's play mode: `openField.SetUrl(new VRCUrl(...))` and the machine's `OpenLink`
  event. The editor cannot download a test file, so an editor hook hands the machine the PNG
  as a texture when its `fetchState` becomes loading (`hostImage`, `fetchW`, `fetchH`,
  `fetchStatus` 200, `fetchState` 3). The same file arrived there with the same sum, which
  is the control pass reading a file's bytes out of an sRGB texture exactly.
- Not tried: a real download in VRChat itself, which needs the picture on a public https
  host. VRChat's own texture may differ from the editor's (its colour space, a size limit).
