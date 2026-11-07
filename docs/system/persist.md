# Persist: a folder the host keeps between sessions

Everything the machine writes is in memory and gone at power-off. One folder is the
exception: `/persistence`. When asked, the guest packs it and hands the bytes to its host,
which keeps them; after the next boot the host hands them back and the guest unpacks them.
In the VRChat world the host keeps them as the player's PlayerData, 100 KB a player and
world, so the folder is for settings and small saved games.

Saving is asked for, never done by itself: with the SAVE button of the sign on the computer,
with Settings' Storage tab, or with `nxpersist save`. Loading is done once a session, when
the desktop starts, and can be switched off. What is in
the folder and not saved is lost with the rest.

    nxpersist load | save | clear | auto 0|1 | state        (load!: though loading at start is off)

## What is in it

| Folder | What | How it gets there |
|---|---|---|
| `registry` | every program's settings, a line a key: below | `registry.h` |
| `desktop/` | where the desktop's icons stand | the `nx` script links `/root/Desktop/.places` there |
| `doom/` | `.doomrc`, `doomsav*.dsg` | the starter runs the game there, with `HOME` there |
| `quake/` | `config.cfg`, `*.sav` | the starter's `id1` is a link to it |
| `quake3/` | the settings | the starter's `fs_homepath` |
| `vanilla-conquer/` | Tiberian Dawn's and Red Alert's settings and saved games | the starters' `XDG_CONFIG_HOME` |
| `classicube/` | `options.txt` | a link from the game's folder |
| `nxsolitaire/` | `game`, the game in play when the window closed; `stats`, the statistics | `nxsolitaire` itself (`docs/apps/solitaire.md`) |
| `micropolis/` | saved cities, `*.cty` | Micropolis's Save and Save as (`docs/games/micropolis.md`) |
| `openttd/` | `openttd.cfg` (some 10 KB), `save/*.sav`, not compressed (92 KB a new 64 by 64 map, 30 KB packed with its folder; 246 KB and 49 KB at 128 by 128; 1.2 MB and 226 KB, too much, for `busy.sav`). `save/autosave` and `screenshot` are links to `/tmp` (7 November 2026: one autosave of a busy map, 1.2 MB, left the folder too large to save) | the starter's `XDG_CONFIG_HOME` and `XDG_DATA_HOME` (`docs/games/openttd.md`) |
| `aoe/` | `savegame/*.gam`, stored as they are since 7 November 2026 (an early game 1,751,130 bytes, packed to 69,211; the game's own deflate made 175,663, which nothing packs further and which is over the limit alone), the AI's learning (`learn/`); its logs and pictures of the screen are `/tmp/aoe-scratch`'s | the starter's `AOE_HOME` (`docs/games/aoe.md`, "Saved games") |
| `vanilla-conquer/` (Red Alert) | `SAVEGAME.NNN`, stored as they are since 7 November 2026 (the game compressed and enciphered them, which left nothing to pack) | `docs/games/ralert.md`, "Saved games" |

The desktop has an icon for the folder, and anything else put there is kept as well: Files'
and the desktop's menu has Save to persistence for an entry, which copies it there and refuses
when the folder would then hold more than 100,000 bytes, saying the sizes
(`docs/system/nanox.md`, "Save to persistence"). Not in it: ClassiCube's maps (262 KB each
before packing), the NES's settings, and the picture file a wallpaper names (the registry keeps
its path, the file itself; one of the image's pictures is there at the next session, a file of
the user's only if it was put in `/persistence` too, or the desktop says it is gone and shows
a colour).

## The registry

What a program wants kept is not a file of its own but a key of one registry that all
programs share: `/persistence/registry`, a line a key, `program.name=value`. `registry.h`
(`linux/apps`, a header and nothing to link) has `reg_get`, `reg_int`, `reg_set`,
`reg_set_int` and `reg_changed`; `nxreg get | set | unset | list` is the same for a script.
A set is in the file at once: under a lock, made of the file as it is then, written beside
it and put in its place, so two programs' keys do not undo each other (forty keys set by
many programs at once all stay) and no reader finds half a file. `reg_changed()` says
when another program has written it: the desktop looks once a second and takes a new
wallpaper so. The file is 16 KB at most and a value 255 bytes; without `/persistence` a set
fails and the program goes on with what it has. Like the rest of the folder it lasts beyond
the session only when the folder is saved.

| Keys | Whose | What |
|---|---|---|
| `desktop.wallpaper`, `desktop.size` | Settings, the desktop, the `nx` script | a picture's path or `#RRGGBB`; `WIDTHxHEIGHT` |
| `sound.volume` | Settings, the `nx` script | 0 to 100 |
| `wm.place.PROGRAM`, `wm.remember` | the window manager | where a program's first window opens (`docs/system/nanox.md`, "The window manager and the bar"); 0: not kept |
| `wm.maximise_games` | Settings (Look), the bar, the desktop | 0: a game started from the Start menu's Games or its desktop shortcut opens as it asks, not maximised (default 1) |
| `desktop.style`, `desktop.font`, `desktop.color` | Settings (Look), the `nx` script | the windows' style, the text, and the windows' colour (`RRGGBB` or `default`: `docs/system/nanox.md`, "Looks") |
| `files.details`, `files.order` | Files | the list or the icons; the list's order |
| `rays.width`, `rays.height`, `rays.turns`, `rays.shadows` | `nxray` | its bar's settings (a test with `RAY_FRAMES` takes none of them) |
| `pathtrace.detail`, `.samples`, `.turns`, `.background`, `.background.colour`, `.live` | `nxpath` | the picture's settings |
| `nxsolitaire.draw` | `nxsolitaire` | 1 or 3 cards turned from the stock |
| `nxperf.TEST` | `nxperf` | a test's last result: its value and `1`, `0` or `-` (its check held, did not, not run); `--cli` sets them only with `--save` (`docs/apps/bench.md`) |
| `micropolis.speed`, `.autobudget`, `.disasters` | `nxmicropolis` | the speed (1 to 3), "Budget by itself", disasters on |
| `aoe.eula`, `jk2.eula` | `cabinst` (the two games' starters) | the CRC-32 of the licence accepted, read out of the installer: it is not asked again while it is the same text (`docs/system/install.md`) |
| `web.home`, `web.pictures`, `web.bookmarks`, `web.bookmark.N` | the browser | its home page, whether pictures are loaded, and up to 24 bookmarks, `ADDRESS TITLE` each (`docs/apps/web.md`) |

Not in it: the games' own settings and saved games (their files, in their folders above),
where the desktop's icons stand (a file of lines), and whether the folder is loaded at start
(the host's: it is asked before anything is loaded).

## The bytes

`tar` of the folder, after 16 bytes: `SEPD`, a version, the count of the bytes that follow, their
CRC-32. A save makes version 3 (7 November 2026); versions 1 and 2, which saves made before, load as ever.

**Version 3: LZMA in blocks on the worker cores.** The archive is cut into blocks of 256 KB, and each
block is raw LZMA2 by itself, with the 256 KB before it (or all there is) as its preset dictionary: what
it would find in one stream of the whole is found, but every block is a job of its own, on up to 16
workers (memory, not cores, limits them: each has an arena of some MB for its encoder, made and touched
by the first core). An archive of one block is packed on the first core, a piece at a time for the
window's bar. After the header: the archive's size, the block's, the dictionary's before a block, the
count, a word a block (its packed length, the top bit set when it was packed as text), then the blocks.
A load unpacks them in order on the first core, each with the bytes before it as its dictionary.

- The encoder is hash chains (`hc4`, nice length 64, depth 24), not the binary tree: a worker's stores go
  through its write cache (`docs/machine/multicore.md`), and the tree's took a worker three times as
  long. Literals go by the byte before (`lc` 3) in a block that is mostly text, by none (`lc` 0) in one
  that is not (zeros, an archive's headers and padding, are not counted).
- `NXPERSIST_FORMAT=1` or `2` saves as before, `NXPERSIST_BLOCK`, `_PRIME`, `_WORKERS`, `_MF` (`bt4`),
  `_NICE` and `_DEPTH` change version 3's choices, and `nxpersist test FILE` packs a file as a save
  packs the archive and unpacks it again: the sizes, the seconds and whether it came back the same.
- Without the memory for the blocks, a save is made as before: version 1, a gzip file at level 1, and
  for a folder that does not fit so version 2, an xz file of one stream with no sum of its own and
  then a word, what it unpacks to (its dictionary the folder's size, 2 MB at most, halved down to
  256 KB when the encoder finds no memory; with none at all deflate at level 9). A folder that does
  not fit even then is refused, and one that level 1 left at twice the limit is refused at once.

Measured on `rvc_cpu` with the card's clock (`--pass-model`, `RVC_WCACHE=1`, `RVC_ADFAULT=1`), 7 November 2026:

| What | Bytes | Deflate level 1 | Version 2 (one stream) | Version 3 |
|---|---|---|---|---|
| a tar of `/etc`, the Start menu's and `open`'s lists (mostly text) | 227,840 | 49,285 in 1.7 s | 38,768 in 20.0 s | 38,890 in 24.5 s (one block); 39,553 in 9.6 s in blocks of 64 KB |
| an Age of Empires save, stored as it is | 1,751,130 | | 69,504 in 150.4 s | 69,211 in 57.7 s |
| two of them | 3,511,808 | 406,634 in 22.6 s | 72,248 in 238.5 s | |

Blocks with no dictionary before them lost too much (the save 88,628 in blocks of 256 KB) and the
binary tree on the workers too much time (67,133 in 143.5 s); blocks of 128 KB with 384 KB before them
were slower still (256.7 s). The 1.2 MB of OpenTTD's `busy.sav` is 137 KB at best (LZMA preset 6, `lc`
0) and fits in no setting: filters of its map's rows and columns of its records made it larger.

`nxpersist` hands over 96,000 bytes at most. A folder that packs to more is not saved: the
window says by how much and names the three largest files, and what the host kept before
stays. A loaded blob whose mark, count or sum is wrong is not unpacked.

## Guest and host

The guest asks with the fetch's words (`docs/system/fetch.md`) and an address no network has; the
host answers these itself, with a fetch's status.

| Address | The host | Status |
|---|---|---|
| `persist:load` | puts what it keeps into the fetch's 256 KB | 200; 404 nothing kept; 403 loading at start is off |
| `persist:load!` | the same, whatever the switch says | 200, 404 |
| `persist:save` | reads from the fetch's 256 KB as many bytes as the request's offset word says, and keeps them | 200; 413 over 100,000; 500 not kept |
| `persist:clear` | keeps nothing any more | 200 |
| `persist:auto=0`, `=1` | loading at start off, on | 200 |
| `persist:state` | answers with the text `stored N`, `limit N`, `auto 0|1`, a line each | 200 |

For a save the guest stores its bytes at `0x816c0000` first, which until now only the host
wrote. The text field in front of the screen is asked for the same way (`clip:`, `docs/system/clipboard.md`). The host's SAVE button reaches the guest as an address to open (`docs/system/open.md`):
`open persist:save` runs `nxpersist save`, so it needs the desktop's bar.

The `nx` script runs `nxpersist load` after the window system and before the desktop and the
bar, once a session (`/tmp/nxpersist.loaded`); a kept screen size other than the running one
starts the desktop over once. With a window system `nxpersist` shows a window with what it is
doing and a bar; without one it prints lines.

## Hosts

- The world: `EmuPersist` (PlayerData keys `persist.blob` and `persist.auto`, read only after
  `OnPlayerRestored`, which `EmuGate` hears for it while it is off, until the visitor leaves the
  corridor they arrive in: `EmuGate.restored`), `EmuMachine.PersistStep`, and `PersistRead.shader`, which a save draws
  once: the guest's bytes out of the state, a pixel a word, read back into a byte array.
  "Build world" makes the behaviour, its target and the sign over the power button
  (`Persist`; 6 November 2026: before, only a menu of its own did, and a rebuilt world kept nothing). A world without the sign
  answers 404 at once.
- `rvc_cpu --persist FILE` and `rvc_harness --persist FILE` (`rvc_persist.h`): the bytes are
  the file, `FILE.auto` holds the switch. These and `host:name` (below) are the only addresses
  `rvc_cpu` answers. Without the option both say 404.

## In a real client

In the editor's play mode, the world's machine on the card: the guest's first load is answered
404; a file and a registry key written at the console, Save pressed on the sign, says "Saved.
Kept: 1 KB of 100." and status 200; with play stopped and started again the sign says what is
kept before the machine is on, the load is 200, the file and the key are there. "Load at start"
goes off and on, Clear asks twice.

In a VRChat client, by `persistTest` on `EmuPersist` (set in the scene, "Build & Test", read
the client's `output_log`: its lines are `[PersistTest]`; turned off again before the scene
is saved). The client turns the machine on, reads the folder at the console, writes a file,
presses Save, starts the machine again and reads the file, which must say `from-vrchat`.

A client of "Build & Test" keeps no PlayerData from one start of the client to the next (it
restores "from 0 buckets", and writes nothing when it is closed): that the folder outlasts
a session can only be seen in an uploaded world.

## Checked, and not

`tools/test_all.py apps-persist-harder apps-persist-big apps-persist-blocks apps-persist-host`: a
folder deflate leaves over the limit fits, one of 9 MB is refused, 700,000 bytes in blocks on the
workers (and as one stream) come back the same (`--races`: 0 undone), and a folder saved to the host
(`rvc_cpu --persist`), taken away and loaded again has the same sums. `tools/persist_blob.py make FOLDER
BLOB` makes a blob of version 1 that a run's `nxpersist load!` unpacks (a way to bring files into a run
without making an image), `read` and `info` say what a blob holds.

On `rvc_cpu`, a private image: a folder saved in one run is there with the same sums after
the next start; the file's header, sum and archive read by Python on the host; a flipped byte
is refused; a folder that packs too large is refused with its sizes and the file is left;
`auto 0` stops the start's load and `load!` does not mind; `clear`; Settings' three buttons by
keys; each game's starter leaves its files in the folder. Through `rvc_harness --cpu --warp`: a
save, loads small and large with the file's sum after, and the windows' frames with their bars
part-way. The test set's `nxfiles-persist` has Save to persistence only.

Not done: the folder across two sessions of an uploaded world; VRChat's own count of the
bytes, and whether one key may hold 100 KB; a save of more than a few kilobytes on the card;
Doom's and Quake's files after a quit by hand; a restart for a kept screen size; a headset.

## The player's name

The address `host:name` of a fetch is answered by every host with the name of whoever the
machine is theirs: the world with the visitor's VRChat display name (printable ASCII, the rest
`_`), the harness and `rvc_cpu` with `SHADEREMU_PLAYER`, else the computer's user, else
`player`. `nxpersist name` prints it. The desktop's starter (`nx`) keeps it, the letters,
digits, `.`, `_` and `-` of it and 32 at most, in the registry's `user.name` and exports it as
`USER` and `LOGNAME` to every program of the desktop: the bar shows it before its speed's box,
Quake is started with `+name`, Quake III with `+set name`, ClassiCube with it as the
launcher's name (`launcher-cc-username` in its options). A program that wants an identifier
for its player reads `$USER`, or the registry's key outside the desktop. On `rvc_cpu`,
`SHADEREMU_PLAYER` with a space in it gives a `user.name` without the space, and Quake III enters
under that name.
