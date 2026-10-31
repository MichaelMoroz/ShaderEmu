# Persist: a folder the host keeps between sessions

Everything the machine writes is in memory and gone at power-off. One folder is the
exception: `/persistence`. When asked, the guest packs it and hands the bytes to its host,
which keeps them; after the next boot the host hands them back and the guest unpacks them.
In the VRChat world the host keeps them as the player's PlayerData, 100 KB a player and
world, so the folder is for settings and small saved games.

Saving is asked for, never done by itself: with the SAVE button of the sign on the computer,
with Settings' Storage tab, or with `nxpersist save`. Loading is done once a session, when
the desktop starts (on a machine whose worker cores `geotune` lays out, at boot, just before
that: `docs/machine/multicore.md`, "Packing the small workers"), and can be switched off. What is in
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
| `micropolis/` | saved cities, `*.cty` | Micropolis's Save and Save as (`docs/games/micropolis.md`) |
| `openttd/` | `openttd.cfg` (some 10 KB), `save/*.sav` (14 KB a 64 by 64 map) | the starter's `XDG_CONFIG_HOME` and `XDG_DATA_HOME` (`docs/games/openttd.md`) |

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
| `files.details`, `files.order` | Files | the list or the icons; the list's order |
| `rays.width`, `rays.height`, `rays.turns`, `rays.shadows` | `nxray` | its bar's settings (a test with `RAY_FRAMES` takes none of them) |
| `pathtrace.detail`, `.samples`, `.turns`, `.background`, `.background.colour`, `.live` | `nxpath` | the picture's settings |
| `micropolis.speed`, `.autobudget`, `.disasters` | `nxmicropolis` | the speed (1 to 3), "Budget by itself", disasters on |
| `web.home`, `web.pictures`, `web.bookmarks`, `web.bookmark.N` | the browser | its home page, whether pictures are loaded, and up to 24 bookmarks, `ADDRESS TITLE` each (`docs/apps/web.md`) |
| `geotune.width`, `.workers`, `.iterations`, `.us`, `.ratio`, `.force` | `geotune`, at boot | the small workers' blocks' width, and what it was chosen by (`docs/machine/multicore.md`, "Packing the small workers"); `.force` 2, 4 or 8 is a width with nothing measured |

Not in it: the games' own settings and saved games (their files, in their folders above),
where the desktop's icons stand (a file of lines), and whether the folder is loaded at start
(the host's: it is asked before anything is loaded).

## The bytes

`tar` of the folder, deflated as a gzip file by zlib inside `nxpersist`, after 16 bytes:
`SEPD`, a version (1), the count of the bytes that follow, their CRC-32. It is not BusyBox's
gzip, which has no levels. Level 1 is the default; level 6
is tried only for a folder that level 1 leaves just over the limit.

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
wrote. The host's SAVE button reaches the guest as an address to open (`docs/system/open.md`):
`open persist:save` runs `nxpersist save`, so it needs the desktop's bar.

The `nx` script runs `nxpersist load` after the window system and before the desktop and the
bar, once a session (`/tmp/nxpersist.loaded`), unless `geotune` loaded it at boot
(`/tmp/nxpersist.early`); a kept screen size other than the running one
starts the desktop over once. With a window system `nxpersist` shows a window with what it is
doing and a bar; without one it prints lines.

## Hosts

- The world: `EmuPersist` (PlayerData keys `persist.blob` and `persist.auto`, read only after
  `OnPlayerRestored`, which `EmuGate` hears for it while it is off, until the visitor leaves the
  corridor they arrive in: `EmuGate.restored`), `EmuMachine.PersistStep`, and `PersistRead.shader`, which a save draws
  once: the guest's bytes out of the state, a pixel a word, read back into a byte array.
  "ShaderEmu/Add the storage sign to the open scene" makes the behaviour, its target and the
  sign over the power button; after "Build world" it is run again. A world without the sign
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
