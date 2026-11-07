# Export and import: `nxexport`

A file or folder taken out of the machine as a photo VRChat's camera takes, which a PC opens as a zip, or as text a
visitor copies out of the world; and both brought in again: a link to such a photo opened in the machine, or text
pasted in front of the screen. `linux/apps/nxexport.c`.

    nxexport PATH ...            the Export window (the file manager's menu: Export... on the selection)
    nxexport --import [FOLDER]   the Import window: the field in front of the screen asked for text (Import... on a folder)
    nxexport --clipboard         the desktop's clipboard shown in that field     (the Start menu has both)

## Export

The window's choices:

| Choice | What |
|---|---|
| To | **Photo (a zip)** (the default): drawn into the pictures of VRChat's camera (below); **Text (base64)**: the field in front of the screen (`docs/system/clipboard.md`) and the clipboard |
| Packed as | **As it is** (one file, its bytes: the default for one file), **Zip** (the default for a folder or many entries), **tar.gz**, **tar.xz** (the smallest); a photo is always a Zip |
| Level | 0 to 9: zlib's for Zip and tar.gz (Zip's 0 is stored), xz's preset for tar.xz, its dictionary no larger than what it packs (xz's own preset 9 asks 674 MB) |
| Text as | **Base64 only** (the default): what a computer outside the world decodes as it is; **data: address**: the name and type kept |

### A photo (7 November 2026)

The developer's idea: VRChat's camera, put into a cube before the screen, gets a picture that is the file.

- **What the visitor does.** Show the photo; a glowing cube pulses before the panel (45 cm at the wall's display; at a visitor's classroom place, where the panel stands before
  the place's screen at 0.32 of its size, the cube stays 30 cm: `EmuClip.CubeSize`) in front of the screen (everyone
  sees it, `PhotoCube.shader`). Open VRChat's camera, hold it inside the cube and take a picture: it shows coloured
  noise, which is the file. On the PC the picture is in `Pictures\VRChat` as a PNG: open it in Paint, Save as, BMP
  picture (a 24-bit bitmap), rename the `.bmp` to `.zip` and open it. The panel says this (`EmuClip.ShowPhoto`), and the
  window too. A link to the PNG opened in the machine gives the zip back (below, "Import").
- **Only the camera's picture.** `Photo.shader` draws one triangle over the whole picture only while
  `_VRChatCameraMode` (VRChat's global: 0 the player's own view on a desktop or in a headset, 1 and 2 the handheld
  camera in VR and on a desktop, 3 a screenshot) is not 0, `_VRChatMirrorMode` is 0, and the camera's place is inside
  the renderer's cube. The player's own view is never covered, and the cube is not drawn in the picture taken from
  inside it.
- **The bytes.** A 24-bit bitmap as Paint saves one is 54 bytes of header, then the pixels' rows from the bottom,
  blue, green and red, no padding when the width times 3 is a whole number of words (every camera size: 1280, 1920,
  2560, 3840). So the shader's pixel (x, row from the bottom) holds the file's bytes from 54 + (row x width + x) x 3.
  The file is a zip: its entries from byte 54, with every offset counted from the file's start (as a self-extracting
  archive has them), zeros, and its directory and end at the file's very end. The one word that depends on the
  picture's size, where the directory begins, the shader makes from `_ScreenParams`; so the guest need not know the
  camera's size, and any size the zip fits works. One it does not fit, or whose rows Paint would pad, is red.
- **What the guest gives.** `nxexport` packs the zip with the entries' offsets 54 further (`zip_shift`) and the
  directory's start 0, copies it to memory aligned on 16 bytes and asks `photo:show=TEXEL,ENTRIES,TAIL,NAME` (the
  first texel of RAM from 0x80000000, the entries' and the directory's bytes). The world's `EmuMachine` gives it to
  `EmuClip.ShowPhoto`, which sets the material: the state texture, the texel (column and row: RAM's texels start at
  row 64), the lengths as 16-bit halves (a material's numbers are floats). `photo:hide` when the window stops it,
  goes to another target or closes.
- **How large.** A 1920 x 1080 picture is 6,220,800 bytes, 2560 x 1440 11,059,200, 3840 x 2160 24,883,200; the
  window says before packing which size the zip fits whatever it packs to ("A zip of it fits a photo of 1920 x 1080
  or larger"). A damaged picture (a JPEG, a copy that was recompressed, one scaled) fails the zip's CRC-32s: it never
  gives wrong files.
- **Checked.** On `rvc_cpu` (`RVC_PHOTO=FILE`, `RVC_PHOTO_SIZE=WxH`: `harness/apps/rvc_persist.h` makes the bitmap by
  the shader's sums, `photoByte`): a folder of three files and 4 MB of random bytes at 1920 x 1080, 7 MB at 3840 x
  2160 (and refused at 1920 x 1080), at 1280 x 720 and 2560 x 1440; each bitmap made a PNG and saved again as a BMP
  (Python's PIL, as Paint), the same bytes, then opened as a zip by Python's `zipfile` and by Windows'
  `Expand-Archive`: every CRC right, every file's md5 the guest's. In Unity 2022.3 the shader itself drew the same
  bytes into a camera's 1920 x 1080 sRGB target, with HDR off, on, and with 4x MSAA: the PNG made a BMP was the
  harness's bitmap with 0 bytes differing. `test_all.py export-photo` holds it. Not yet tried: VRChat's own camera in
  a client (its colour handling as the editor's camera's is what the test above assumes) and Paint itself (PIL writes
  the same 54-byte header and rows).

### Text

**For a computer outside the world.** The text is base64 alone by default: on a PC, click the field, Ctrl+A, Ctrl+C,
paste into Notepad and save it as `data.txt`, then in a command prompt `certutil -decode data.txt NAME` (Windows, nothing
to install; Linux and macOS `base64 -d data.txt > NAME`): the field's line above the text and the window say it with the
file's name. Showings of the field after the first are cut on whole groups of 4 letters with nothing added, so their
texts joined in order in one file are the text. Tried: a tar.xz of three files exported in the guest, its 15,920
characters decoded by Windows' `certutil`, every file's md5 the guest's. Base64 alone has no name: an import here names
it `imported` and an ending by its first bytes.

The other form is a data: address, `data:TYPE;name=NAME;base64,DATA` (a name's other signs as `%XX`): pasted into a
browser's address bar it saves the file (browsers do not keep its `name=`; this machine's Import does), and any base64
decoder takes what follows the comma.

The text goes to the field in parts of at most 256 KB (the fetch's memory); the window's Next part shows the next.

### Before packing, and bars

**What it will be, before packing.** The window walks the selection once when it opens (bytes, files, names) and says
under the choices, before anything is packed, the most characters the text can be and how many showings of the field
that is, or the camera's size a photo needs: conservative bounds that hold whatever the files hold. Zip: the files
stored (deflate that makes a file no smaller stores it) and every entry's headers; tar.gz and tar.xz: busybox's tar of
the same at most (512 bytes a header, files rounded to 512, long names' extra entries, a whole 10 KB record) through
zlib's `deflateBound` formula for any level and liblzma's `lzma_stream_buffer_bound`; as it is, the file. `--text`
prints the bound beside the text made, and `export-text` fails on `BOUND BROKEN`.

**Bars.** Packing shows `ui_progress.h`'s window after 0.7 s: deflate and LZMA fed 64 KB at a time against the files'
bytes, busybox's tar against its archive's bound as it grows; closing it stops the packing. An import's unpacking has it
too. A zip is of the selection's names from their common folder, folders' entries and modes kept; symbolic links are
left out. tar.gz and tar.xz are busybox's `tar` of the same, packed in the program (zlib, liblzma).

QR codes were a target until 7 November 2026 (Project Nayuki's generator): a code holds 1,370 characters at most, and
the photo holds megabytes, so they went. An import still takes an older export's codes' `K/N:` parts.

## Import

**A photo, by its link.** `open` (`linux/apps/open.c`, `photo_like` and `photo_zip`) given an address whose picture
the host hands over as pixels (`docs/system/fetch.md`) looks at its first part: the top row is the would-be bitmap's
last bytes, and when they end with a zip's end whose directory fits a file of the picture's size, every pixel is kept,
the bitmap's bytes made of them, and the zip cut out (its entries, its directory, their offsets 54 less) and saved as
`NAME.zip`, which `open` then gives to the archive program by its kind. So a photo's PNG put on any image host and its
link handed to the world (the links panel) comes in as the zip. VRChat's picture loader takes pictures up to 2048 x
2048: a photo of 1920 x 1080 comes this way, a larger one not. Checked under `rvc_harness --cpu --host-file` with a PNG
of `export-photo`'s bitmap: `/tmp/open/d.zip`, 112,371 bytes, an archive, its four files the zip's.

**Text.** The field in front of the screen is asked for (`clip:ask`) and asked every second for what was given with
To the machine (`clip:take`), ten minutes at most. The text is a data: address (its name kept), base64 alone (named
`imported` and an ending by its first bytes), or an older export's parts of codes (each `K/N:` taken away, the rest
joined; line ends are dropped); anything else is text, saved as `pasted.txt`. The file goes into the folder under a
name not taken ("NAME (2)"), and a zip, a tar.gz or a tar.xz is unpacked into a folder beside it named as the file
without its ending (unarc; an archive of one folder of that name is that folder's entries), which the window says.

## For tests

    nxexport --pack as|zip|tgz|txz LEVEL OUT PATH ...    packs: "nxexport: packed NAME as KIND: N bytes, sum S"
    nxexport --text OUT PATH ...                         the text the window makes (NXEXPORT_PACK, NXEXPORT_LEVEL, NXEXPORT_FORM=plain|data),
                                                         and "before packing at most N characters ..., bound held"
    nxexport --photo PATH ...                            a zip shown as a photo: "photo of NAME: E bytes of entries and T of its
                                                         directory, W x H at most, sum S: STATUS" (NXEXPORT_PHOTO_HOLD=S keeps it S seconds)
    nxexport --decode TEXTFILE FOLDER                    an import of that text
    nxexport --push | --paste                            the clipboard to the host (clip:set), the host's text onto it
    NXEXPORT_GO=1 NXEXPORT_TO=photo|text NXEXPORT_PRINT=1   the window's choices, Export pressed

`tools/test_all.py export-text` (a folder through each packing's data: address and back, `diff -r` the same; one file as
it is; a zip as base64 alone into `imported`; every bound held), `export-photo` (the bitmap `rvc_cpu` makes is a zip of
the guest's files, its members' md5s the guest's: the case's `zipped` check; a zip too large for 1920 x 1080 refused),
`export-clip`.

Not done: an import of more than 256 KB of text at once (the fetch's memory: paste and give it in parts); a photo of
more than one picture.
