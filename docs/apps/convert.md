# The converter: `nxconvert`

`nxconvert` makes a picture, a sound or a film into another kind, "something like ffmpeg with a window". It reads
and writes everything through libmedia (`docs/system/media.md`), on the worker cores, and is in the Start menu as
Utilities, Converter. The file manager's menu on a picture, a sound or a film has **Convert...**, which starts it with
that file (`linux/apps/nxfiles.c`, `convert_entry`).

    nxconvert [FILE]                         the window
    nxconvert --info FILE                    what the file is, a line
    nxconvert --to FORMAT [options] IN OUT   a conversion without a window; prints what it made and the sum

| Of a | It makes |
|---|---|
| picture (PNG, JPEG, GIF, BMP, PNM, TGA) | JPEG (a quality), PNG (a level, 1 to 9), GIF, BMP, PPM, TGA, at a size |
| sound (WAV, FLAC, MP3, Ogg Vorbis, MOD) | WAV (16 bits), FLAC |
| film (MPEG-1, VQA, MJPEG in AVI, an animated GIF) | MJPEG in AVI (a quality, with its sound or without), an animated GIF, its sound alone as WAV or FLAC, every Nth picture as pictures (`NAME-0001.jpg` on), at a size |

Options: `--quality Q` (JPEG's and AVI's 1 to 100, 85 when not given; PNG's level, 6), `--scale PERCENT` or `--size WxH`,
`--every N` (a film's pictures taken for a GIF or for pictures), `--no-sound` (an AVI without), `--overwrite`. Without
`--overwrite` a file that is there is not replaced: "not made: a file of that name is there already". The line printed
is `nxconvert: made OUT: what, N bytes, sum XXXXXXXX, T ms`, the sum FNV-1a over the bytes written (over every picture
in turn for a film's pictures), as `media_sum`.

## The window

Open... (or `o`) chooses a file (`ui_files.h`); under it is what the file is ("MPEG-1 film, 320 x 176, 480 pictures at
24.00 a second (20.0 s), sound 32000 Hz stereo, 894,976 bytes"). A row of buttons is what it can make (pictures: JPEG
PNG GIF BMP PPM TGA; sound: WAV FLAC; films: AVI GIF WAV FLAC JPEGs PNGs); under them the quality or level, the size (10
to 200%, the size it makes beside it), for a film's GIF or pictures every how many, for an AVI the sound. Save as is
beside the file with the new ending, and never a file that is there (`Fox.png`, `Fox-2.png`, ...); Name... asks for
another (`ui_save_file`, which asks before it replaces one). Convert (or Enter) works with a bar and a line saying
what it is doing (a picture: decoding the first half of the bar, resizing and encoding the rest, each told by libmedia's
loops on the workers as their pieces end; a sound: decoding, then encoding); Escape stops it, and so does closing the window. Escape otherwise closes it. After a conversion the name offered is the next free one,
so a second Convert never writes over the first. Choosing GIF for a film sets 50% and every second picture: an
animation of every picture at full size is seldom wanted, and one over 48 MB of pixels is refused.

`NXCONVERT_PRINT=1` prints where each button is in the window and what a Convert made; `NXCONVERT_WORKERS=N` the
workers (as many as the machine has, 63 at most, otherwise); `NXCONVERT_TIMES=1` where a film's time went.

## How it works

- **Pictures**: decoded on the workers (`media_image_decode`), fitted to the size asked (`media_image_fit`), encoded by
  libmedia's encoders (`media_jpeg_encode` and the others). See-through pixels are kept for PNG, GIF and TGA when the
  picture has any, and laid over white for the others.
- **Sound**: every piece decoded on the workers (`media_audio_decode_all`; a MOD in order), then WAV
  (`media_wav_encode`) or FLAC (`media_flac_encode`, its blocks on the workers).
- **Films**: the sound first, whole: the film decodes it ahead by itself (`media_video_sound`). Then the pictures in
  **batches**. While a film is open its workers are its own and the program posts nothing to them (`media.h`), so
  the pictures of a batch (up to 96, or 24 MB) are copied out as the film gives them, the film is closed, and the
  batch is encoded on every core; then the film is opened again and sought to where it was (`media_video_seek` goes to
  the group's first picture: the pictures before are decoded again and dropped). An AVI's batch is
  `media_avi_film`: each picture made pixels, fitted and encoded as a JPEG by a core of its own, side by side, the
  chunks then put in order with each picture's sound after it. A GIF's pictures are kept and encoded at the end
  (`media_gif_anim_encode`), a picture file's are written one by one.

The program opens its workers for a conversion and closes them after (`mcw_open`), so every call of the library and
the film lend them.

## Checked

`python tools/test_all.py convert-pictures convert-sound convert-film convert-window` (tags `media`, `apps`,
`desktop`), 7 November 2026 on `rvc_cpu`, every one passing:

| Case | What | Sums (FNV-1a of the file) |
|---|---|---|
| `convert-pictures` | `picture-fox.jpg` (640 x 480) to PNG, JPEG, GIF, BMP, PPM, TGA; `picture-fox.png` to JPEG at 50%; a name that is there refused | `5b38ecc9` 227,682 bytes, `bd0b8027` 34,374, `3cae98ae` 78,798, `0a40b790` 921,654, `57d14865` 921,615, `397b512a` 921,618, `5d2ed007` |
| `convert-sound` | `play-test.mp3` to FLAC (63 workers and none: the same bytes), `play-test.flac` to WAV, the AVI's sound to FLAC; both FLACs decoded again | `8ca12520` 2,079,890 bytes (twice), `6bcc8ea3`, `33990159`; decoded `aa07a2e5` (the MP3's samples) and `caab0fc3` (the AVI's) |
| `convert-film` | `Big Buck Bunny.avi` to AVI, `.mpg` to AVI at quality 75, `.mpg` to a GIF at 50% of every fourth picture, every 48th picture of the AVI as JPEG; the AVIs played again by `media_test video` | `4e0789ff` 757,272 bytes, `2c5a85f1` 5,689,068, `b10560b8` 488,671 (120 pictures), `13f75eba` (2 pictures); the AVIs' pictures `26ebe6f9`, `396b3d97`, their sound `6dd01fde`, `96a296a2`: the sources' own |
| `convert-window` | `nano-X`, `nxconvert /tmp/f.jpg`, PNG and Convert clicked by `nxkey`, Escape | `/tmp/f.png` `5b38ecc9`, as the command line's; the program gone after Escape |

The pictures' files are byte for byte what `media_test-host encode` makes of the same picture on the build computer;
the MPEG-1's AVI is what `media_test-host filmenc` makes (pictures one at a time over every core, not side by side:
the same bytes). ffmpeg reads the AVIs (`ffprobe`: `mjpeg` 320 x 176 at 24/1, 480 pictures; `pcm_s16le` 32 kHz stereo)
and decodes every picture; against ffmpeg's own decoding of the MPEG-1, the AVI at quality 75 is 43.1 dB PSNR (Y
44.2, U 41.9, V 41.0), and the MJPEG AVI at 75 against the AVI it came from 45.3 dB. `RVC_RACES` over the picture,
sound and film conversions above (and the film at 50%, an animated GIF and an AVI of it): 0 changes undone, every sum
as without it. `test_all.py --races` (`RVC_RACES_NEAR=4`) on the four cases: 0 undone; near, two changes in one
texel of the MPEG-1 decoder's sound jobs (`decode_chunk` and `plm_audio_decode` on two workers, `m_video_mpeg.c`),
not looked into; the GIF encoder's trailer, which the first core stored beside the last page's codes, is now that
page's core's.

## What it costs

Calibrated `rvc_cpu` (`--ticks 12288 --pass-model 1.196921,0.040689,0.001677,1.659333,0.27`, `RVC_WCACHE=1
RVC_ADFAULT=1`), 7 November 2026, 63 workers, the program's own `T ms`:

| Conversion | Seconds |
|---|---|
| `picture-fox.jpg` to JPEG, GIF, BMP, PNG (level 6) | 0.46, 0.45, 1.32, 2.08 |
| `play-test.mp3` (18 s) to WAV, to FLAC | 4.32, 6.45 (the FLAC's encoding 2.1 s of it; with no workers 125 s) |
| the MPEG-1's sound (20 s) to WAV; that WAV to FLAC | 6.45; 2.18 |
| `Big Buck Bunny.avi` (96 pictures) to AVI | 6.72: decoded 1.68, encoded 4.44 |
| `Big Buck Bunny.mpg` (480 pictures, 20 s) to AVI | 46.8: the sound 4.34, decoded 9.00, opened again 4 times 2.58, encoded 27.4 (one picture at a time over every core, `NXCONVERT_ONE_BY_ONE=1`: 34.0) |
| the same to a GIF at 160 x 88, every fourth picture | 29.4 (the animation's colours and codes about 15 of it) |

A film is some 2.5 times slower than it plays. The pictures' encoding is most of it: a picture of 320 x 176 is made
pixels (the YUV's colours), fitted and encoded on one core, and with 96 pictures a batch over 64 cores each core has
one or two.

## Not done

- Pictures of a GIF or of a film's pictures are encoded one after another (each over every core), not side by side as
  an AVI's.
- A film's GIF has every picture's own colours (`media_gif_anim_encode`): no rectangles of what changed.
- Opening a film again for each batch decodes its group from the start (2.6 s of 46.8 for the MPEG-1).
- Other kinds of film, and MPEG-1 written: `docs/system/media.md`, "Other kinds of film".
- Not tried on the card, nor the window on the real desktop (`nx`) beyond `nano-X -p`.
