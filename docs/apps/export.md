# Export and import: `nxexport`

A file or folder taken out of the machine as text a visitor copies out of the world, or as QR codes a phone
scans off the screen; and text pasted in front of the screen brought in again as a file. `linux/apps/nxexport.c`.

    nxexport PATH ...            the Export window (the file manager's menu: Export... on the selection)
    nxexport --import [FOLDER]   the Import window: the field in front of the screen asked for text (Import... on a folder)
    nxexport --clipboard         the desktop's clipboard shown in that field     (the Start menu has both)

## Export

The window's choices:

| Choice | What |
|---|---|
| Packed as | **As it is** (one file, its bytes: the default for one file), **Zip** (the default for a folder or many entries), **tar.gz**, **tar.xz** (the smallest) |
| Level | 0 to 9: zlib's for Zip and tar.gz (Zip's 0 is stored), xz's preset for tar.xz, its dictionary no larger than what it packs (xz's own preset 9 asks 674 MB) |
| To | **Text (base64)**: the field in front of the screen (`docs/system/clipboard.md`) and the clipboard; **QR codes**: shown in the window |
| Codes | **Big** (QR version 10 at most, 57 modules: 201 characters a code), **Medium** (20, 97: 654), **Dense** (30, 137: about 1,300); error correction M |

What is made is one text, a data: address, `data:TYPE;name=NAME;base64,DATA` (a name's other signs as `%XX`):
a browser given it saves the file under its name, and any base64 decoder takes what follows the comma. A zip is
of the selection's names from their common folder, folders' entries and modes kept; symbolic links are left out.
tar.gz and tar.xz are busybox's `tar` of the same, packed in the program (zlib, liblzma).

The text goes to the field in parts of at most 256 KB (the fetch's memory); the window's Next part shows the
next. One QR code holds the text alone; more each hold `K/N:` and a part, the text again when joined in order
without those. The window shows a code across most of itself (8 pixels a module for a big one in its 560 x 520),
Previous and Next or the arrows, and Back.

On `rvc_cpu`, 7 November 2026, a folder of `/etc` (63 entries): Zip at 9 53,238 bytes (71,025 characters, 109
medium codes, 354 big), tar.gz at 9 40,113, tar.xz at 6 36,244; the dense codes of the tar.xz, 36 of them.
jsQR (a decoder that is not the generator) read the window's code off the harness's screen, a big one
(`data:text/plain;name=hostname;base64,cnZjCg==`) and the first dense one (`1/36:data:application/x-xz;...`).
Python's `zipfile`, `gzip` and `lzma` opened the three packings the guest made.

## Import

The field in front of the screen is asked for (`clip:ask`) and asked every second for what was given with
To the machine (`clip:take`), ten minutes at most. The text is a data: address (its name kept), the parts of
QR codes (each `K/N:` taken away, the rest joined; line ends are dropped), or base64 alone (named `imported`
and an ending by its first bytes); anything else is text, saved as `pasted.txt`. The file goes into the folder
under a name not taken ("NAME (2)"), and a zip, a tar.gz or a tar.xz is unpacked into a folder beside it named as
the file without its ending (unarc; an archive of one folder of that name is that folder's entries), which the
window says.

## For tests

    nxexport --pack as|zip|tgz|txz LEVEL OUT PATH ...    packs: "nxexport: packed NAME as KIND: N bytes, sum S"
    nxexport --text OUT PATH ...                         the text the window makes (NXEXPORT_PACK, NXEXPORT_LEVEL)
    nxexport --qr big|medium|dense TEXTFILE              the codes of a text: how many, and a sum of every module
    nxexport --decode TEXTFILE FOLDER                    an import of that text
    nxexport --push | --paste                            the clipboard to the host (clip:set), the host's text onto it
    NXEXPORT_GO=1 NXEXPORT_TO=qr|text NXEXPORT_CODES=big|medium|dense NXEXPORT_PRINT=1   the window's choices, Export pressed

`tools/test_all.py export-text` (a folder through each packing's text and back, `diff -r` the same; one file as
it is), `export-qr` (the codes' sums: medium 1 code, big 3, of a file of the image), `export-clip`.

The QR codes are Project Nayuki's generator (MIT), fetched by `linux/apps/build.sh` at its tag v1.8.0.

Not done: a code's text in the alphanumeric mode (base64 has small letters, so a code is in bytes); QR codes in
a way a scanner joins by itself (structured append, 16 codes at most, which phones' cameras do not join); an
import of more than 256 KB at once (the fetch's memory: paste and give it in parts).
