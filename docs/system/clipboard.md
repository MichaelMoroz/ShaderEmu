# The text field in front of the screen

A visitor of the VRChat world cannot copy anything off the machine's screen: it is a picture. What they can
copy is the text of a field of the world's own, and what they paste there is text the world can hand to the
machine. One field does both (`EmuClip`): it stands before the wall's display, or before the visitor's own
classroom place's tube while they have one (as the links panel does, `EmuMachine.PlaceLinks`). The guest asks
for it with addresses of the fetch's (`docs/system/fetch.md`) that no network has, as `persist:` is asked for
(`docs/system/persist.md`).

| Address | The host | Status |
|---|---|---|
| `clip:show` | reads as many bytes from the fetch's 256 KB as the request's offset word says: a line of 256 bytes (what the field says above the text, ending in a zero), then the text; shows them | 200; 413 over 256 KB |
| `clip:set` | the same bytes, all text: the machine's clipboard, kept and shown when the Clipboard key is pressed | 200 |
| `clip:ask` | shows the field empty, for a paste | 200 |
| `clip:take` | what the visitor gave with To the machine, into the answer, once | 200; 404 nothing yet |
| `clip:hide` | the field put away (not the clipboard's) | 200 |

A host without the field (a world built before it, a harness without `RVC_CLIP`) answers 404 to all of them.
`host_ask.h` (`linux/apps`, a header) is the guest's asking: `host_open()`, `host_free()` before leaving bytes
in the fetch's memory, `host_ask(ADDRESS, BYTES, SECONDS)`, `host_answered()`.

## In the machine

- **Export** (`nxexport`, `docs/apps/export.md`) shows its text with `clip:show`, at most 256 KB a showing
  (more is shown in parts, the window's Next part), and puts it on the clipboard too.
- **Import** (`nxexport --import [FOLDER]`, the file manager's Import...) asks with `clip:ask` and then asks
  `clip:take` once a second, for ten minutes at most; what comes is a file of the folder, and an archive is
  unpacked beside it. `clip:hide` at its end.
- **The clipboard** is `/tmp/clipboard` (`ui_clip_set`, `ui_clip_get` in `ui.h`; `docs/system/nanox.md`). The
  desktop's bar looks at it once a second and, when it has changed, starts `nxexport --push`, which hands it to
  the host with `clip:set` (the bar does not wait for the host itself). `nxexport --clipboard` shows it in the
  field at once (the Start menu's "Clipboard in front of the screen").
- **To the machine** of the clipboard's field: the host tells the guest to open `clip:paste`, as its Save is
  told (`docs/system/open.md`, "From the host to the guest"); the bar runs `nxexport --paste` for an address of
  `clip:`, which takes the text with `clip:take` and puts it on the clipboard. An import's own field is not told
  so: its `nxexport` asks by itself, and the two would race for the one paste.

## In the world

`EmuClip` (`unity/ShaderEmu/Scripts/EmuClip.cs`), made by "Build world" with its panel ("Clip panel",
`ShaderEmuClip.cs`; it moves, so it is never made a static plate: `Moves` in `ShaderEmuRetro.cs`):

- `EmuMachine.FetchAsked` hands every `clip:` address to `ClipStep`, which waits for a show's or a set's
  readback of the fetch's memory (`PersistRead.shader` into a 256 x 256 target: the whole 256 KB) as a save of
  the kept folder waits. The bytes are UTF-8 (`Encoding.UTF8`, which Udon has).
- The field is a legacy `InputField` of many lines. A `Text` draws at most 16,383 characters (four vertices a
  character), so the text is shown in pages of 8,000 (`< Page`, `Page >`, "Page 2 of 9"); a visitor copies a
  page at a time (click, Ctrl+A, Ctrl+C). What is pasted is kept whole whatever the field draws.
- The Clipboard key is on the console keyboard's strip, the display keyboard's strip, and a classroom place's
  strip ("Station fields", for its owner: `EmuStations.Clipboard`). Close puts the field away.

Not tried: any of it in Unity or VRChat. The Udon scripts and the builder compile with Roslyn against the
project's assemblies (UdonSharp's own rules not checked); nothing has been built, baked or played with them, and
whether VRChat's keyboard in a headset pastes into a legacy `InputField` is not known.

## In the harnesses

`RVC_CLIP=FILE` (`harness/apps/rvc_persist.h`, both `rvc_cpu` and `rvc_harness`): `clip:show` writes the text
to FILE and its line to FILE.title, `clip:set` writes FILE.board, `clip:take` gives FILE.in (and deletes it:
what a visitor would have pasted), and `RVC_CLIP_IN=FILE2` gives FILE2 once a run and keeps it, for a test.
`rvc_harness` reads eight rows of the state for a show or a set on the card.

`tools/test_all.py export-clip` hands the clipboard over both ways and imports `tools/clip/paste-zip.txt` (a
data: address of a zip of two files) into a folder: `hello.txt` says what it should.
