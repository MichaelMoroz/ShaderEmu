# Fonts

The window system has two fonts of its own, a bit a pixel, Latin letters only. A page of the
web wants more: other scripts, faces with and without serifs, a fixed one, bold, italic, and
sizes from a footnote's to a heading's. Those are files in the ROM that the GPU draws glyphs
from where they lie; the processor's part of a string is still a word a character.

    python tools\make_fonts.py            # into build\images\linux\root\usr\share, before the image is made
    python tools\make_fonts.py --check build\images\linux\root\usr\share\font-sans-13.sef

| File | What |
|---|---|
| `font-sans-N.sef`, `font-sans-b-N.sef` | Liberation Sans, regular and bold |
| `font-serif-N.sef`, `font-serif-b-N.sef` | Liberation Serif |
| `font-mono-N.sef`, `font-mono-b-N.sef` | Liberation Mono |
| `font-uni-16.sef` | GNU Unifont: every character of the basic plane, 8 or 16 pixels wide |
| `font-ui-N.sef`, `font-ui-b-N.sef` | Selawik, regular and bold: the desktop's text in its smooth look (`docs/system/nanox.md`, "Looks") |
| `font-NAME.msdf` | the face's distance fields, for every size of it ("Distance fields") |

- **The desktop's faces** are [Selawik](https://github.com/microsoft/Selawik) 1.01 (Microsoft's
  open stand-in for Segoe UI, SIL Open Font License), at 11, 12, 13, 15 and 17 pixels
  (`UI_SIZES`); `python tools\make_fonts.py --ui [FOLDER]` makes only these. Each glyph has at
  least a pixel after its ink (`gap=1` in `outline_font`), or a colon at 12 pixels touches what
  follows it. `font-ui-12.sef` is the programs' and the captions' text
  (`UI_LOOK_FONT` in `linux/apps/ui_look.h`): height 15, ascent 12.
- The window manager's caption draws in it when the look is smooth (`caption_title` in the
  patch's `nxpaintnc.c`); the screen driver's `romfont_draw` fills a background when the GC
  asks for one and drops the glyphs a clip would cut through, as the bitmap fonts do.

- N is the size in pixels: 10, 11, 13, 15, 17, 20, 24, 28, 32. A text takes the nearest.
  The outline faces have Latin (with its extensions and Vietnamese), Greek, Cyrillic,
  punctuation, arrows, mathematical signs and the box-drawing characters: about 1,450 glyphs
  each, rasterised by FreeType with the faces' own hinting (through Pillow), kept as four bits
  of coverage a pixel. Unifont is a bit a pixel and has the rest: Chinese, Japanese, Korean,
  Arabic, Hebrew, the Indian scripts, 57 thousand glyphs. 79 files, 15.1 MB of ROM (31 October 2026).
- The fonts are fetched once into `build\fetch\fonts` (Liberation 2.1.5, SIL Open Font
  License; Unifont 16.0.04, GPL 2+ with the font exception and OFL) and are not kept in this
  repository, like the desktop's pictures.
- Italic is the upright face slanted by the GPU (a fifth of a pixel a row), not a face of its
  own. A size that is not in the list is the nearest scaled ("Distance fields"). Unifont alone is
  drawn at twice its size, for text of 26 pixels and more.

## The file

`linux/nanox/sef.h`, little-endian, every part at a multiple of 16 bytes so that the GPU's
texels fall on it wherever the file lies in the ROM (romfs keeps a file's data at a multiple
of 16):

| Part | Contents |
|---|---|
| header, 64 bytes | `SEF1`; a line's height and ascent; bits a pixel (1, 4, or 24 for distance fields); glyphs; where the parts are; the glyph for a character the font lacks; the fields' range |
| ranges | runs of code points, rising: first, count, first glyph (12 bytes each) |
| advances | a byte a glyph |
| table | a 16-byte texel a glyph: where its bitmap is in the file, its width, its height, and how far its corner is from the character's (a signed byte each way) |
| bitmaps | rows of whole bytes, the left pixel in the high bits; each glyph begins at a multiple of 4 |

A glyph is its ink's rectangle only: the table says where that lies in the character's cell.

## Drawing

`docs/machine/gpu.md`, the text command: with the flag `0x100` in its fragment word it has three more
words, a scale (16.16), a slant, and what the table's bitmap addresses are counted from (the
file's own place in the ROM, which is not known when the file is made). Fragment mode 7 is
coverage: sixteen levels, which are the colour's alpha, so such text is drawn in a pass that
blends (pass 5). The table and the bitmaps are read from the ROM by the vertex and the
fragment shader as textures and models are.

- **The window system** (`romfont_create`, `romfont_draw` in `scr_shaderemu.c`): a font whose
  name ends in `.sef` is such a file, found in the ROM as a picture is (`rom_address`) and
  mapped for its ranges and advances. `GrCreateFontEx(PATH, height, slant, NULL)`: a height
  other than the file's scales it, the width is the slant in 256ths of a pixel a row.
  Its text is 16-bit characters (`GR_TFUC16`); a string is one command however long.
- **What is drawn after blended text in a list is drawn in the same pass** (`blended` in the
  driver): the passes are drawn one after another, and a fill queued after a line of text
  would otherwise come out under it. The list's first command, the surface's own picture,
  stays in the opaque pass.
- A font not in the ROM (a file in memory), or a server that draws in software
  (`NANOX_SOFTWARE=1`), has none of this: the name is no font, and the program gets the
  built-in one. Text of these fonts is not clipped to a window's children: a program draws
  it where it is all its own (the browser draws into a pixmap).
- **A program measures by itself** (`web_advance` in `linux/apps/web.h`): it maps the same
  file and adds the advances up; asking the server would be a question a word.

## Distance fields

The outline faces are also made as multi-channel signed distance fields: one atlas a face
that draws at any size, sharp, instead of a bitmap a size that a scale makes blocky.

    python tools\make_fonts.py [OUT]               # distance fields for every size (the default)
    python tools\make_fonts.py [OUT] --bitmap      # bitmaps only, for A/B
    python tools\make_fonts.py [OUT] --msdf-from 15   # bitmaps below 15 pixels, fields from there

- `font-NAME.msdf` is the face's atlas: a 64-byte header (`SEM1`, its pixels an em and its
  distance range in those pixels, glyphs, and the sizes made of it with their lines' heights:
  `struct sef_atlas`), then each glyph's box of three bytes a pixel, rows not padded, from a
  multiple of 4. Made by [msdf-atlas-gen](https://github.com/Chlumsky/msdf-atlas-gen) 1.4 by
  Viktor Chlumsky (MIT licence; the Windows build is fetched once into `build\fetch\msdf`),
  `-type msdf -size 24 -pxrange 4` (`ATLAS_EM`, `ATLAS_RANGE`).
- `font-NAME-N.sef` keeps everything a program reads, the same as the bitmap file: header,
  ranges, the hinted advances FreeType gives at N, a table a glyph. Its bits a pixel is
  `SEF_MSDF` (24), word 11 of the header (`range`) the atlas's range in pixels at N (16.16),
  and its table is into the atlas: where the glyph's pixels are, their width | height << 16,
  and the quad's width | height << 16 and corner x | y << 16 (signed) in 64ths of a pixel at N.
  A quad's corners are the centres of the box's corner pixels, and the outline is put in the
  middle of its hinted advance (half the rounding each side: else small text's gaps are
  uneven). Lines, widths and layout are those of the bitmaps: `web_advance`,
  `tools/web_test.c` and the browser's counts are unchanged.
- The desktop's faces have 20, 24 and 32 pixels too (`UI_LARGE`, 6 KB a size), so that large
  text is scaled from hinted advances near its own size, not spaced by a small size's rounding.
- The GPU draws such a glyph in fragment mode 8 (`docs/machine/gpu.md`): a pixel's coverage is the
  mean of four samples, one a quarter of its box (2 x 2), each the four atlas pixels round it,
  weighed, the median of their channels the distance, and the quarter's coverage that distance
  times twice the range in the screen's pixels, which the vertex stage works out a command
  (word 12 times the scale, times `MSDF_SHARP`, 1.2): no derivatives. A pixel's pair is two or
  three words of the ROM.
- **Small sizes are fitted to the hinted bitmaps.** At up to 17 pixels (`FIT_UP_TO`) each
  glyph's quad is shifted and stretched across and down, by up to half a pixel and 12%, until
  the outline's coverage of each pixel is nearest the bitmap FreeType hints at that size
  (`fit_quads` in `tools/make_fonts.py`): stems and the baseline land where the hinting puts
  them, as one pixel and not two half-covered ones. The outline is not thickened. Sizes from 20 are the outline as it is.
- **The window system** finds the atlas beside the file (`msdf_nearest` in `scr_shaderemu.c`);
  without it the font is none and the program gets the built-in one. A height asked of
  `GrCreateFontEx` takes the face's size whose line is nearest and scales it: sizes are not
  snapped, and 48 pixels of the desktop's face is the 32's fields at 1.5 times. The browser builds its own lists
  (`list_text` in `nxweb.c`) and needs the same three words: mode 8, the atlas, the range.
- `tools/web_test.c` draws such glyphs as the GPU does, on the host, four samples a pixel too.

Not done: kerning, ligatures, the joining of Arabic letters and the reordering of text
written from the right (such text is shown, each letter in its own form, from the left),
characters outside the basic plane (emoji), a real italic.
