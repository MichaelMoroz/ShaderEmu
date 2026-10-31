# libmedia: pictures, sound and films on the worker cores

`linux/libmedia` is the one library every program of the machine decodes and encodes media with:
pictures (PNG, JPEG, GIF and its animations, BMP, TGA, PNM), sound (WAV, FLAC, MP3, Ogg Vorbis,
MOD) and films (MPEG-1, GIF, Westwood's VQA, Motion JPEG in AVI). Every format that can be cut into parts that
stand by themselves is decoded on the worker cores (`multicore.md`); what cannot, is pipelined
with them. A program links `/usr/lib/libmedia.a` with `mcw.c` and `mcwx.c` and includes
`media.h`.

    wsl -- bash linux/nommu/build.sh libmedia/build.sh    (8 s; after the kernel's)

The build also makes `media_test` (in the image, `/usr/bin/media_test`) and the same program for
the build computer (`$WORK/src/libmedia/media_test-host`, workers as threads:
`test/mcw_host.c`), whose sums the guest's must equal. `linux/tcc/build.sh`, which makes the
image's `/usr/lib` and `/usr/include` anew, keeps the library and `media.h` there: build this first.

| Who uses it | For |
|---|---|
| the desktop (`nxfiles --desktop`, `ui.h`) | its picture: a JPEG decoded when it starts, into a pixmap the GPU keeps |
| Settings (`nxsettings`) | the desktop's picture, of any kind |
| the viewer (`nxview`) | every kind; animations played |
| Paint (`nxpaint`) | opening every kind; saving PNG, JPEG, GIF, BMP or PPM by the name's ending |
| `nxicon` | an icon from a picture of any kind |
| the music player (`nxplay`, `play.md`) | WAV, FLAC, MP3, Ogg Vorbis, MOD |
| the video player (`nxvideo`, `video.md`) | MPEG-1, Westwood VQA (Red Alert's movies), MJPEG in AVI, GIF animations |
| Red Alert (`ralert.md`) | its movies' block rows and pointers (`media_vq4_rows`, `media_lcw`) |
| ClassiCube (`classicube.md`) | its texture pack, PNGs again |
| the browser (`nxweb`, `web.md`, "Pictures") | the image's JPEGs and PNGs of the web and the host's pictures, decoded to the size drawn into the GPU's memory |

`workers` in every call is the most worker cores it may use. A program with no workers open has
them opened for the call and closed after; a program that has its own lends them (and gets them
back with no team of the library's left on them). The first call of a program reads its text
once before any worker runs it: a worker stops at every page of code no core has run.

Lent workers may be busy with the program's own jobs (Quake III's server builds bot routes on
one), so the library never posts to a worker by number: every step is a loop (`mcw_for`), which
passes a busy worker by. A pipeline (a JPEG's or a PNG's one stream: this core reads, workers
follow) is a loop too, `m_stages`: item i on the i-th core of the team, each core's items in
order, so an item may wait for lower ones and the lowest unfinished one always runs; core 0
has item 0, the reading. With 20 of 63 workers held by calls of the program's own
(`MT_BUSY=20`) the fox's PNG in one stream takes 6.39 s against 6.41 with all free, its JPEG
without markers 1.21 against 1.49.

## Pictures

    struct media_image out = { 0 };
    out.channels = 4, out.bgr = 1;                  /* 0x00RRGGBB words, the window system's */
    why = media_image_decode(data, size, &out, mcw_most());    /* pixels mapped for it */
    why = media_image_decode_fit(data, size, &dst, cover, workers);   /* to dst's size */
    file = media_jpeg_encode(&image, 90, &bytes, workers);        /* or png, bmp, ppm */

Pixels are 3 bytes (a PPM's: what the GPU draws in fragment mode 4) or 4 (red first, or blue
first with `bgr`), rows `stride` apart. `opaque` lays see-through pixels over `background` in 4
channels too. `media_image_fit` scales pixels in memory to another size (half or less: the mean
of the pixels under each; else the four nearest its middle, weighed), covering it or fitting
inside, rows on every core. `media_image_to_ppm` decodes a file into a PPM file mapped for
writing; without an MMU, where a file cannot be mapped so, the picture is made in memory and
written after (`docs/system/nommu.md`).

`media_progress`, when a program sets it, is called on the first core while a picture is
decoded: done of total from the JPEG decoder's rows of blocks, a PNG's inflated bytes (one
stream, every 32 KB) and the rows of `m_for`'s loops the first core runs (the fitting's
among them), or 0 of 0 while the first core waits on the workers. Often, and from different
stages: the program keeps the largest share and draws when it likes (`ui_decoding_told` in
`linux/apps/ui.h`).

What is parallel in each:

| | How the work is shared | The file's own help |
|---|---|---|
| JPEG with restart markers | every core finds the markers in a part of the file, then each takes whole **restart intervals**: its codes, transform and colours, and writes its MCUs' pixels | an interval stands by itself |
| JPEG without | the scan in parts, a part a core, read three times over all cores (below, "Without markers"): from a guessed start, from where the part before ended, and for where each piece of an MCU row begins; then the pieces, some four a core, each read and drawn by itself | |
| progressive JPEG | this core reads every scan into the coefficients; then strips on every core | |
| PNG in **segments** | every core inflates and unfilters segments of its own, in place; then the pixels over every core | the `mdPR` chunk (below) |
| PNG in one stream | this core inflates; workers unfilter a band of every row each, one behind the other, and the last writes the pixels | |
| interlaced PNG | this core inflates; the seven passes are unfiltered a core each; the pixels gathered over every core | |
| GIF | every frame's LZW on a core of its own, all of them ahead; a frame laid on the canvas in rows over every core | a frame stands by itself |
| BMP, TGA, PNM | rows over every core (runs unpacked on this core first) | |
| PNG encoding | segments of 16 rows or more, each filtered and deflated by a core of its own; the checks put together here (`crc32_combine`, `adler32_combine`) | |
| JPEG encoding | some 512 restart intervals, each encoded by a core into memory of its own | |
| GIF encoding | the colours gathered by every core from rows of its own, or the fixed ones dithered by rows; LZW in pieces of rows, a piece a core; the sub-blocks written by every core, a page each | a clear code starts the codes anew |

A core writes only its own memory: 16 rows of pixels are whole 16 bytes whatever the width, an
MCU is whole 16 bytes of every row it covers when the output's rows are (then intervals are the
jobs; otherwise strips of 16 rows, each read from the start of the interval it begins in).

**A JPEG shown smaller** (`media_image_decode_fit`): one shown at half its size or less is decoded
at a half, a quarter or an eighth of it, the largest that is still at least the size shown, and
then fitted. At an eighth a block is its mean (no transform); at a half and a quarter each sample
is the mean of the transform's under it, so the colours and stores are four or sixteen times
fewer. Against Pillow's decoding scaled down by area: within 0.4 levels at 4:4:4, within 1.5 to
4.2 at 4:2:0, where a colour sample covers 16 pixels (the reference: Pillow over the whole blocks, as a smaller decoding is).

**Files laid out for the workers.** A JPEG in restart intervals and a PNG in segments are any
reader's files, and this library decodes them wholly in parallel:

- JPEG: a restart marker every so many MCUs (`DRI`). This library's encoder writes some 512
  intervals a picture; `tools/make_wallpaper.py`, `tools/make_media.py` and
  `tools/make_test_pictures.py` ask Pillow for the same (`restart_marker_blocks`). The cost is
  under 1% of the file (the fox's desktop picture: 126,036 bytes plain, 127,927 in intervals).
- PNG: each IDAT holds rows of their own, in a deflate stream that refers to nothing before it
  and ends on a byte (an empty stored block, as zlib's full flush leaves), its first row
  filtered without the row above; the private chunk `mdPR` (ancillary, private, unsafe to copy)
  is the number of segments and each one's first row, big-endian words. This library's encoder
  and `linux/libmedia/png_segments.py` (for the build's tools) write them.

**GIF encoding** (`media_gif_encode`, `m_gif_enc.c`; `media_image_save` and Paint by `.gif`): a
picture of 256 colours or fewer keeps them, in the colours' order, and see-through pixels (4
channels, red first, alpha under 128) take one more; one of more takes 252 fixed colours, 6 red
x 7 green x 6 blue, with a 4 x 4 ordered dither: three tables a pixel and no pass over the whole
picture's colours on one core, which a median cut would want. The codes are pieces of rows of
16,384 pixels or more, each begun as after a clear code and encoded by a core of its own. A
piece ends with a clear code at the width it has got to, then clears at the first width until
it ends on a byte; the minimum code size is made even, so that first width is odd and some
number of clears under 8 always comes to a byte. The pieces are then bytes put one after
another (in sub-blocks of 255, written by every core a page each), and a decoder sees one
stream that clears now and then. The pieces are by the picture's size, not the workers': the
same file from any number of them. They cost some 5% of the file (the fox at 1,600 x 1,200 in
256 colours: 446,932 bytes, 426,342 in four pieces).

**Animations** (`media_gif_anim_encode`): each frame is the canvas whole, encoded as above over
every core with its colours as a table of its own, after a control block with its delay and its
see-through index (a frame with see-through pixels is cleared after it is shown, so that the one
before does not show through; another is left in place), and a NETSCAPE2.0 block with the loop
count. `media_test animenc IN.gif OUT.gif N` writes a GIF's frames again;
`python linux/libmedia/test/check_gif_anim.py MEDIA_TEST --wsl` makes a pan over the fox (12
frames, 320 x 240, a palette of its own each) and dots on see-through ground (8 frames, played 3
times, cleared after each) with Pillow: every frame the same pixels, the delays and loop counts
the same, the same bytes from 0, 4 or 16 workers; 0 bad. On `rvc_cpu` the pan takes 32.3 s on
one core and 6.54 s with 63 workers (a frame of 76,800 pixels is five pieces), the file the
host's byte for byte, `RVC_RACES` 0 undone.

### Checked

`python linux/libmedia/test/check_pictures.py MEDIA_TEST --wsl` makes pictures of every kind
(333 x 221: sizes not of whole MCUs) and compares this library's decoding with Pillow's, in 3
and 4 channels with 4 workers: PNG of every colour type and depth, palettes with `tRNS`,
interlaced, 16 bits; JPEG 4:4:4, 4:2:0, 4:2:2, grey, progressive, with restart markers by rows
and by blocks, CMYK; GIF; BMP of 1, 8, 24 and 32 bits; PNM P4 to P6; TGA plain, RLE, grey and
with alpha. PNG, GIF, BMP, PNM and TGA are the same pixels; JPEG within what two decoders'
colour stretching differs by (a mean of 0.03 levels at 4:4:4, under 0.8 with colours at half
size, where Pillow's stretching is smoothed and this one's is the nearest sample). An
animation's frames, each way of leaving a frame among them, have the sums of Pillow's. The
encoders' files are read back by Pillow: PNG (levels 1, 6 and 9), BMP and PPM the same pixels; this library's JPEG
at quality 90 is Pillow's own at that quality, coefficient for coefficient (the same sum when
decoded). GIF of 200, 16 and 2 colours and of grey are the same pixels, and the full-colour fox
the fixed colours as `gif_fixed` in the test dithers them, pixel for pixel. 0 bad.

`RVC_RACES` under `rvc_cpu` over the fox's JPEG in intervals, its plain and progressive JPEGs, its
PNG in segments and in one stream (16 workers), an animated GIF (8), a fit to 400 x 266 and both
encoders: 0 changes undone, every sum as without it; the reads "as it was" (3.7 million) are cores
waiting on another's count.
The same over the fox's plain, interval and uneven JPEGs (markers crowded into a third of the
picture, more than a part of the scan has room for: found again on this core) and its PNG in one
stream, with 63 workers of the library's and 63 lent of which 20 busy (`MT_OPEN=63 MT_BUSY=20`),
and the coefficient list (`MEDIA_JPEG_LIST=1`) so: 0 undone, the sums equal.
The GIF encoder over 1,600 x 1,200 in 256 colours, 333 x 221 in 200 and the fox's desktop picture
(fixed colours), 16 workers: 0 undone, the files the same bytes as the build computer's.

### What it costs

The desktop's picture, 1623 x 1080 (the fox), under `rvc_cpu` with every pass a round of 8,192
instructions a core (`--ips 1e12 --pass-ms 2.7307`: workers count as cores side by side), into
pixels mapped beforehand, the workers already open (`MT_OPEN=63 MT_PREALLOC=1 MT_REPEAT=3
media_test decode FILE OUT 63 4`; the second decoding of three):

| | One core | 63 workers |
|---|---|---|
| JPEG in restart intervals (this library's or `make_wallpaper.py`'s) | 44.6 s | 0.82 s |
| JPEG without restart markers (Pillow's plain file) | 44.6 s | 3.55 s before the guesses (4.0 with the list); see "Without markers" |
| progressive JPEG | 75.7 s | 24.7 s |
| PNG in segments (this library's, filters None, Sub, Up) | 69 s | 2.45 s |
| PNG in one stream (Pillow's) | 95 s | 28.7 s |
| PPM (the copy alone) | 20.9 s | 0.40 s |
| encoding: JPEG at quality 85 | 88 s | 2.3 s |
| encoding: PNG at level 1 (32 workers at most) | 251 s | 16.3 s |
| encoding: PNG at level 6 | 484 s | 26.8 s |
| encoding: GIF, the fox at 1,600 x 1,200 in 256 colours (workers opened in the call) | 48.2 s | 2.03 s (16 workers: 4.0 s) |
| encoding: GIF, the fox's desktop picture (fixed colours) | 38.0 s | 1.98 s (3.18 s) |

- A JPEG's instructions are some 60 a pixel: the colours 30 (from tables, the same numbers),
  the transform 25, the codes 5. A progressive file waits for its scans, which one core reads.
- What a first decoding in a program costs more: its workers opened (70 to 320 ms) and, where
  the pixels are mapped in the call, their clearing by the kernel (6.9 MB: 434 ms). The desktop
  decodes into a pixmap that is there already; Paint and ClassiCube touch their own pixels.
- What was taken out on the way, measured on the same picture: the restart markers' places in
  256 mappings of a page (470 ms of every decoding: one block now); strips of 16 rows as the jobs
  where an MCU is whole 16 bytes of every row (2.18 s to 1.28: an interval a job); working memory
  through `mcw_scratch` (a worker's first store to a new page waits for the first core: it is
  mapped by the first core now, or kept from call to call under 1 MB); a PNG segment of 32 rows
  (34 jobs: 16 rows, 68); Paeth's filter in this library's PNG files (most of the decoding: 0.1 to
  1.7% larger without).
- PNG encoding is the slow side: some 430 instructions a pixel at level 1 (filters chosen of
  three by sums, a match looked for at every byte, codes built a block at a time). Paint saves at
  level 1: a 1,600 x 1,200 picture is about 17 s with 32 workers; a drawing, with long matches,
  much less.

### Without markers

A Huffman code finds its own way back: read from any bit, the codes after a few dozen are the
stream's own. So the scan is cut in parts (a core each, a kilobyte at least) and every core
reads its part from a guessed start (the part's first byte, the MCU's first block) to the first
block that begins past the part's end; then, from where the part before's guess ended, again,
counting its blocks and the sums of its DC differences; the first core checks that each guess
ended where the true reading did (a part whose guess had not met the stream by its end is read
once more, on the first core) and sums the counts up; then every core reads its part a third
time knowing each block's number and predictions, and keeps where each piece of an MCU row
begins (pieces of whole 16 bytes, some four a core). The pieces are then read and drawn as the
intervals of a file with markers are. `MEDIA_JPEG_SERIAL=1` is the one core's reading before
it, behind which workers draw strips.

The fox at 1623 x 1080 from Pillow without markers (4:2:0, 155 KB), 3 bytes a pixel, the
pixels mapped before, 63 workers, the second decoding (`media_test_st`, a build of this file
that prints each stage's time):

| | Time |
|---|---|
| one core | 45.3 s |
| one core's reading, workers behind it (`MEDIA_JPEG_SERIAL=1`) | 4.12 s |
| guessed parts: the three readings 93 + 104 + 153 ms, the pieces 1,021 ms | 1.76 s |
| the same picture in restart intervals | 1.08 s |
| 4:4:4 (324 KB): guessed parts, against one core's reading | 2.55 s against 9.55 |
| grey (125 KB): the same | 1.00 s against 3.99 |

Checked: the pixels are the one core's reading's, byte for byte, in 3 and 4 channels, 4:2:0,
4:2:2, 4:4:4, grey and noise at 3, 15 and 63 workers (`media_test-host`); with parts of 40
bytes, so that 137 guesses of 256 had not met the stream by their part's end, the same; on the
machine with 20 of 63 workers busy (`MT_BUSY=20`) the same, and `RVC_RACES` 0 undone.

## Sound

`media_audio_open` takes WAV, FLAC, MP3, Ogg Vorbis and MOD files where they lie in memory and
gives 16-bit frames of one or two channels. A tune is read in order on the first core
(`media_audio_read`, `media_audio_seek`), or in **pieces** that any core decodes by itself, in
any order, with the same samples as an in-order decoding. `media_audio_decode_all` gives every
piece to the workers (`m_for` over the pieces, decoder `mcw_self()` on each core); `nxplay` gives
them out itself, a piece into its place in the sound card's ring (`play.md`).

    m = media_audio_open(file, size, &why);
    media_audio_decoders(m, workers + 1);           /* on the first core: one a core */
    media_audio_piece(m, k, piece, to);             /* on worker k: any piece */
    media_audio_piece_step(m, k, piece, to, done);  /* the same a step at a time, for a job that may be dropped */

| | What a piece is | Made in |
|---|---|---|
| WAV (8, 16, 24, 32-bit integers, 32 and 64-bit floats) | any frames: 8,192 | `m_audio.c` |
| FLAC | whole blocks, of a stream whose blocks are all as long: half a second | `m_audio_flac.c`, over dr_flac |
| MP3 | whole frames, 20 (half a second) | `m_audio_mp3.c`, over minimp3 |
| Ogg Vorbis | any frames: a second | `m_audio_vorbis.c`, over stb_vorbis |
| MOD | none: in order | `m_audio_mod.c` |

`media_audio_set_piece` asks for other lengths (rounded up to whole blocks or frames, and to
whole 16 bytes of output, so two cores never store to the same). `media_audio_half` decodes an
MP3 file at half its rate (minimp3.patch's lower 16 bands): the rate, frames and pieces are
halved. `media_audio_mix_rate` is a tracker tune's mixing rate (22,050 Hz unless asked).

- **WAV**: a piece is a conversion: the top two bytes of an integer, a float rounded to the
  nearest and a half to even (as ffmpeg). The first two channels are kept, as `nxplay` did.
- **FLAC**: a block stands by itself, but its place in the file is written nowhere. A decoder
  finds a block by guesses between two places known: a header found after a guess says which
  block it is (the sync bytes, the header's check byte, a number less than the blocks and the
  block length), and counts when the next header is the block after it. A worker stores nothing
  but its own decoder for that; a decoder that goes on from the block it finished needs no search.
- **MP3**: the table of the file's frames is made when it is opened; a piece's decoder starts 12
  frames early, as `play.md` has it (`MP3D_SKIP`, `MP3D_WARM`).
- **Ogg Vorbis**: a packet's samples are of it and of the packet before. A decoder seeks to its
  piece's first frame (`stb_vorbis_seek`: the page by bisection, then a packet or two decoded for
  their overlap) and reads from there. The codebooks and tables are read once, on the first core:
  a worker's decoder is a copy of the reader's state with its channels' buffers and its temporary
  memory in pages of its own (`stb_vorbis_alloc`), so a worker never allocates (some 40 KB).
- **MOD**: the tracker of `nxplay`, its state in the tune's own memory.

`media_test audio FILE OUT.raw [WORKERS [PIECE]]` (FNV-1a over the bytes; WORKERS -1 reads in
order, -2 decodes the pieces backwards on one decoder, each a seek, -3 also seeks the reader to
200 places and compares) gives the same sums on the build computer (`media_test-host`, built with
the machine's single-precision constants) and the guest, with 0, 16 and 63 workers, in order and
backwards:

| File (25 s of a sweep with noise, unless said) | Sum | Against ffmpeg's `-f s16le` |
|---|---|---|
| `f16.flac`, 16-bit stereo | `9eb75ab7` (= the WAV it was made of) | every sample |
| `f24.flac`, 24-bit stereo | `de49ac9b` (= `w24.wav`) | every sample |
| `play-test.flac` | `518c5ac8` | every sample |
| `w8m.wav`, `w24.wav`, `wf32.wav`, `wf64.wav` | `52d7de09`, `de49ac9b`, `6aec9116`, `6aec9116` | every sample |
| `v44.ogg`, 44.1 kHz stereo, q4 | `b606214b` | 99.79% the same, the rest by 1 |
| `v22m.ogg`, 22,050 Hz mono | `5e1577eb` | 99.81%, the rest by 1 |
| `play-test.mp3` | `aa07a2e5` (`c4627f73` at half rate, build computer) | 99.43% after minimp3's 2,257 frames of encoder delay, the rest by 1 |
| `m22m.mp3`, 22,050 Hz mono (MPEG-2) | `32019b95` | 99.60%, by 2 at most |
| `play-test.mod` | `c714a845` | |

`nxplay`'s own sums (`play.md`, "Checking it") are as they were, with workers and without; the
two Ogg files give `4bc10ffb` and `303b2940`. `RVC_RACES` with 16 workers over Vorbis, FLAC, MP3
and WAV: 0 undone.

Since 28 October 2026 a Vorbis block size's twiddle factors and window are made in floats
(`vorbis_tables.py`, `m_vorbis_sinpi`), not by sines in software doubles: a file's setup was some 18
million instructions on the first core (a third of the Music player's start), now well under one.
The tables are nearer the true sines than before (28,544 values over every block size: at most 5
units in the last place, before up to 713 where the old float angle was rounded), and the samples
move by 1 in a few: the image's two Ogg files 0.05% and 0.12% of their samples, by 1 at most;
`Beethoven - Moonlight Sonata.ogg` with 16 workers `49b4ad58` before, `113e5533` now, the same on
the build computer and the guest. The sums of the table above and of `play.md` are of before
(the files are not in the image).

Instructions for a second of 44.1 kHz stereo (`nxplay`'s `playstat`): Vorbis 15.6 million in
order, 17.6 in pieces (each seek 12% more); FLAC 11.7 and 12.9; MP3 12.9 and 14.0, at half rate
8.6 and 9.6; MOD 1.1; WAV 0.1. `nxplay` decodes the 25 s Vorbis file in 5.5 s with 48 workers,
4.5 times as fast as it plays. (`media_test` decoding whole files under `rvc_cpu`'s own clock,
which counts the workers' instructions one after another: `v44.ogg` 122 s alone, 15.2 with 16
workers, 10.5 with 63, where 25 pieces of a second are 25 cores; `f16.flac` 70.7, 9.4, 6.9.)

## Films

`media_video_open` takes an MPEG-1 file (closed groups of pictures, no B pictures, layer II
sound: `video.md`), a Westwood VQA (Red Alert's and Tiberian Dawn's, version 2, 4x4 or 4x2
blocks, with SND0, SND1 or SND2 sound), Motion JPEG in AVI or a GIF animation, where it lies
in memory, and decodes it ahead on the workers. The first core asks for pictures in order and calls
`media_video_pump` between them; nothing else of the film is its work.

    v = media_video_open(file, size, workers, &why);
    media_video_places(v, places, slots);       /* optional: where pictures go (GPU memory) */
    for (;;) {
        media_video_pump(v);
        have = media_video_ready(v, &in_time);  /* the last picture ready, none missing */
        pixels = media_video_frame(v, media_video_at(v, clock_ms), &palette);
        got = media_video_sound(v, first, to, most);
    }
    first = media_video_seek(v, n);             /* drops what is decoded: n's group's first picture */

| | A picture | Shared out | Made in |
|---|---|---|---|
| MPEG-1 | YUV420: three planes, the GPU makes the colours | a group of pictures a worker, copied into its places a picture a job; the sound in chunks of 14 frames, a job each | `m_video_mpeg.c`, over pl_mpeg |
| VQA | PAL8 and its palette (256 x 0x00RRGGBB) | a frame's block rows over every core; codebooks, pointers and the sound unpacked ahead on others | `m_video_vqa.c`, `m_vq.c` |
| GIF | RGBA, the animation's canvas | every picture's codes unpacked ahead on the workers (`media_anim_open`), a picture laid on the canvas by every core | `m_video.c`, over `m_gif.c` |
| MJPEG in AVI | WORDS: 0x00RRGGBB, the GPU's opaque `GL_RGBA` texels | a picture at a time over every core (its restart intervals), decoded ahead into its place | `m_avi.c`, over `m_jpeg.c` |

- **Places.** A picture the GPU shows must be in GPU memory (a texture has a physical
  address), and that holds few. `media_video_places` gives the library a ring of places:
  picture n is made in place n % slots once the picture there has been let go (asking for
  picture n lets go of those before it, so the one shown keeps its place until the next is
  asked for). Without places the library keeps a ring of its own. A GIF has none: its picture
  is the canvas.
- **One MPEG-1 film at a time** (its jobs find it in one variable); its workers are its own
  while it is open: the program posts nothing to them.
- `media_video_ready` says how far pictures are ready and whether those being decoded will be
  ready in time for a film played on from now (each group going on as fast as it has got to
  where it is: `video.md`, "It plays on when..."). A VQA is always in time.
- `media_video_time` and `media_video_at`: a picture's time and the picture due at a time. A
  GIF's pictures each have their own (its delays); the others' are by `fps_milli`.

### MPEG-1

`linux/video/mpeg.c` and the half of `nxvideo.c` that gave jobs out, moved into the library
as they were (`video.md` has why each thing is so): the file is taken apart on the first core,
a decoder is put back as it was made for every group, a picture is decoded where its group
keeps it, a group is copied into its places a picture a job by its own worker and by every
worker whose group waits, the sound's chunks start the filter's ring where an in-order
decoding has it. pl_mpeg is fetched and changed by `linux/libmedia/build.sh` exactly as
`linux/video/build.sh` did (the same sed, `plm_window.py`, `plm_fast.py`: the same
`pl_mpeg.h`, compared byte for byte). The memory is the film's (mapped, all of it unmapped by
`media_video_close`), not `mcw_alloc`'s, which is never given back. The jobs' words are 32
bits, so a job is given a picture's number, not its place's address (an address on the
build computer is 64 bits).

### VQA

A frame is two bytes of pointer a block into a codebook, and the codebook changes every so
many frames (8 in Red Alert's: a whole one, then the next in eight parts, one a frame). The
game unpacks each part when the frame that needs it is drawn, in a ring of three. Here the
file is taken apart when it is opened: every codebook's pieces (a whole one, or its parts end
to end, packed or not), every frame's pointers, which codebook a frame is drawn with (the last
one whole when the frame's chunks begin: `VQA_LoadFrame`, and the first frame the codebook its
own chunks bring) and its palette (the last one at or before it). So any frame can be made
once its codebook and its pointers are unpacked, and a seek goes to the frame itself.

- **A frame's block rows over every core**, as Red Alert's player does (`ralert.md`, "Movies,
  and the worker cores"): `mcw_for_begin` with the first core idle, a block row of 4 (or 2)
  picture rows a piece, into the frame's place. The pump goes on while the round is out.
- **Unpacked ahead on other workers** (`mcw_try_call`, which takes a worker not in the rows'
  team): the next four frames' pointers (LCW), and the codebooks they need. All but one to
  three workers make rows; the others unpack. Three codebooks are kept unpacked.
- **The sound** is one job from the open on: SND2 (Westwood's 4-bit IMA, the state going on
  from chunk to chunk, `sosCODECDecompressData`), SND1 (`Audio_Unzap`, 8-bit) or SND0, all in
  order into 16-bit frames, counting what it has made; `media_video_sound` gives what is made.
  A pump asks about the job each time: a worker that stops at a page goes on only when the
  first core looks.
- A codebook lies in room for any pointer (two bytes are 65,280 blocks of 16 bytes), mapped
  but not brought in past the codebook's own size: a pointer past it in a damaged file reads
  zeros, as the game would read whatever lay there. LCW stops at either end of its buffers.

`media_vq4_rows` and `media_lcw` are those pieces for a game's own player (`m_vq.c`, which
needs nothing of the library but the workers): Red Alert's `linux/ralert/workers.c` calls them
for a movie's rows and the next frame's pointers, and its build links `libmedia.a` (only
`m_vq.o` comes in).

`media_vq2_rows` is the same for 4x2 blocks (solid where the high pointer is 15): Tiberian
Dawn's drawer calls it for every frame (`tdawn.md`, "Movies"). Both are byte for byte the
game's `UnVQ_4x2` and `UnVQ_4x4` on the build computer: 1,152 random frames of four sizes, at
three places in a page, with 0 to 8 workers, 172.8 million bytes, 0 differ.

### GIF

`media_anim_open` unpacks every picture's codes on the workers when the film is opened;
`media_video_frame` lays the pictures up to n on the canvas, every core rows of it, and
returns the canvas.

### MJPEG in AVI

`m_avi.c` reads the RIFF `AVI ` (and an OpenDML file's `AVIX` after it): the first `vids` stream
of `MJPG` (or `JPEG`, `AVRn`, `dmb1`) and the first `auds` of PCM (8 or 16 bits, one or two
channels), then every `movi` list in the file's order, `LIST rec` too. `idx1` is not read: the
lists say the same. An empty picture chunk is the picture before again; a picture without
Huffman tables (a camera's, `AVI1`) is given the standard ones before its scan.

- **Rounds of eight.** `media_video_pump` takes the next pictures that have a place free (a
  shown picture's), eight at most, in a round of two halves, each out (`mcw_for_begin`, the
  first core taking no part) while the player shows the round before; a later pump starts the
  next half or collects it. First half: each picture on a core of its own reads its tables and
  finds its restart intervals (`m_jpeg_parts`, into a room of the picture's, which keeps its
  count too); one without markers is decoded whole there (`m_jpeg_decode_alone`). Second half:
  all the round's intervals over every core (`m_jpeg_part`), each clearing its words' top byte
  (the decoder writes 0xffRRGGBB; the GPU's top byte is transparency). Every picture stands by
  itself, so a seek goes to the picture itself, once the round out is back. The film needs two
  places, and keeps 25 of its own when the program gives none.
- **Tried and left:** each picture over every core on its own (`m_jpeg_decode`: 8.3 s for the
  sample at 63 workers); a picture a core in rounds (5.97 s, or 4.29 with rounds no larger than
  the team, but a round is a picture's time, 1.3 s, and with 33 places the player still left 2
  out); a call a picture (`mcw_try_call`, joined in order: 25 s at 16 workers, 75 s at 63, for a
  reason not found).
- **The sound** is the file's bytes, copied out of its chunks as it is asked for (8 bits made
  16): all of it is there from the open.
- `tools/make_avi.py` writes such files of Pillow's JPEGs in some 128 restart intervals each
  (`--no-tables` as a camera's, `--sample` the image's `/root/Videos/Big Buck Bunny.avi`).

The sample (320 x 176, 96 pictures, `media_test video FILE N` with `MEDIA_TEST_NOSUM=1`, rounds
of 8,192 instructions a core): 126.4 s with no workers (1.32 s a picture), 11.3 s with 16, 3.44 s
with 63, 28 pictures a second (it was 15.0 and 14.7 a picture at a time over every core).
`nxvideo` with 64 workers and room for 33 pictures plays it with none left out (73 before),
20 to 30 pictures ahead of the film's clock. `RVC_RACES`: 0 undone, with 20 of 63 workers busy
too (`MT_BUSY=20`), sums as above.

### The player

`nxvideo` (`video.md`) is libmedia's: it gives the GPU memory it has as places, shows a
YUV420 picture as before, a PAL8 one as a `GL_COLOR_INDEX8_EXT` texture with its palette
written to `seglPalette()` when it changes, and a GIF's canvas converted to the GPU's words
(0xTTRRGGBB) into two places in turn. A film of one channel is put on both of the sound
ring's. A WORDS picture is a `GL_RGBA` texture where it was decoded. `.vqa` and `.avi` are in its `open` line; a GIF goes to `nxview` by its first bytes, as before, and
`nxvideo FILE.gif` plays it. `nxvideo --decode FILE` decodes with no workers through the
library. `NXVIDEO_SUM=1` sums every picture in its place as it comes there, once, on a film
played from its start, in order: the same sum as `--decode`'s (`d124dc14` for the sample; it
was a sum of groups' sums, `034e8a1b`, when the player knew the groups). `NXVIDEO_COPY` is
gone (the copies are always shared) and the host build `nxvideo-host` with it:
`media_test-host video FILE` gives the sums.

### What it costs

`rvc_cpu` on 15 October 2026 (`--count-from` over `media_test video FILE N` with
`MEDIA_TEST_NOSUM=1`, which asks for every picture in order and sums nothing; instructions,
and the guest's seconds by the interpreter's clock, not the card's):

| | Workers | First core | All cores | The busiest worker | Guest seconds |
|---|---|---|---|---|---|
| ENGLISH.VQA, 160 frames and their sound | 0 | 129.1M (807k a frame) | 129.1M | | 38.9 |
| | 4 | 19.4M | 130.0M | 35.6M | 13.2 |
| | 16 | 18.2M (114k a frame) | 129.0M | 13.1M | 7.4 |
| The sample, 480 pictures and their sound | 0 | 749.7M | 749.7M | | 225.5 |
| | 24 | 59.2M | 754.3M | 32.8M | 19.4 |

All cores together do what one core does alone: the sharing costs no instructions to speak of.
`nxvideo` played both through once with no picture left out (`nxvideostat`), the MPEG-1
sample in 27.2 guest seconds of its 20 (the first core 71.4M), the VQA in 16.4 of its 10.7
(32.1M). `nxvideo --decode` (one core, the copies into places and the sound with the
pictures): 1,867 thousand instructions a picture of the sample, 1,795 of the VQA.

Red Alert with the library's pieces: the opening movie's 156 frames in 15.9 s (the tree's
build 15.5 s, one run each), 329k instructions a frame on the first core (319k), the state
sum at frame 300 the same (`01e0dd60`), `RALERT_MOVIE_CHECK=1`: 0 of 156 differ.

**Real Tiberian Dawn movies.** `python linux/libmedia/test/check_vqa.py MEDIA_TEST FFMPEG
FILE.VQA... --wsl --game` decodes every picture with `media_test` and compares it, made colours by
its own palette, with ffmpeg's decoding to RGB, and the sound with the games' own decoder
(`linux/libmedia/test/vqa_sound_ref.cpp`, vanilla-conquer's `soscodec.cpp` as `vqaloader.cpp`
calls it, which writes FILE.VQA.game.raw). ffmpeg is not the reference for the sound: its IMA
steps are rounded otherwise and differ from the games' by 1 or 2. On `NOD1PRE.VQA` and
`DINO.VQA` (the full game's, 320 x 156 at 15 a second with SND2 sound, from samples.mplayerhq.hu):
32 and 116 pictures all ffmpeg's to the byte, 58,064 and 181,544 samples all the game's; 0 bad.

### Not done

- Tiberian Dawn's movies are `media_vq2_rows`' (`tdawn.md`, "Movies"); its demo has none, and
  the full game's own were tried from the multimedia samples (below). The game's frame is not
  yet the GPU's picture.
- MJPEG: why calls of a picture each were slower than rounds.
- AVI: sound other than PCM, pictures other than MJPEG.
- VQA version 1 (Tiberian Dawn's oldest, 8-bit sound at 22,050 Hz) is taken but not tried;
  stereo SND2 decodes the two channels' bytes in turn, as Westwood's format has them (the
  game's own decoder reads twice the bytes there), untried for want of a file.
- On the card only the AVI, MPEG-1 and the player were timed ("On the card"); VQA, GIF and
  sound times are `rvc_cpu`'s.
- A GIF's canvas is made into the GPU's words on the player's first core.

### Checking it

    media_test video FILE [WORKERS [places]]   # every picture's sum, the pictures' and the sound's

- The sample (`Big Buck Bunny.mpg`): pictures `d124dc14`, sound `96a296a2`, as
  `nxvideo-host --decode` had them, on the build computer and the guest, with no workers,
  with 8 and 24, and into places of the program's own.
- Red Alert's ENGLISH.VQA of the demo (`nchires.mix`, 640 x 400, 160 frames, 22,050 Hz):
  every frame's sum with its palette the same as a reference made of the game's own
  `LCW_Uncompress`, `UnVQ_4x4` and `sosCODECDecompressData` driven as `VQA_LoadFrame` loads
  and the drawer draws (codebooks unpacked in place, in a ring of three), with no workers and
  with 8; pictures `55d71396`, sound `a5e19ed4` on the build computer and the guest.
- A GIF animation's pictures are `media_anim`'s, with workers and without.
- `Big Buck Bunny.avi`: pictures `3129e37f`, sound `6dd01fde` on the build computer and the
  guest, with no workers, 4, 8, 16 and 63, and into places. `python
  linux/libmedia/test/check_avi.py MEDIA_TEST --wsl` writes films of the fox (333 x 221 with
  8-bit stereo sound; 320 x 240 without Huffman tables, 16-bit mono) and has every picture against
  Pillow's decoding of the same JPEG (a mean of 1.03 levels at most, at most 0.4% of the values
  off by more than 24, all at sharp colour edges: the nearest colour sample against Pillow's
  smoothed one), against the still decoder's to the byte, and the sound sample for sample: 0 bad.
- `RVC_RACES` on `rvc_cpu`: 0 undone for the MPEG-1 film (24 workers), the VQA (16) and the
  GIF (4); the VQA's and the GIF's reads "as it was" are the first core waiting on a word.
  The same for the three AVI films (16 and 8 workers, the sample into places).

## Deflate in segments

`media_inflate` takes any file made of pieces that are each a raw deflate stream by itself:
`MDZ1`, the inflated size, the piece's size (a multiple of 16), the count, count + 1 offsets of
the streams, then the streams. Each piece is a job, inflated by PNG's inflate into its place.
Half-Life's pack has its maps, models and sprites so (`linux/halflife/repack.py`, pieces of 8 KB).

    total = media_inflated_size(file, size);            /* 0: not deflate in segments */
    why = media_inflate(file, size, out, total, workers);

`media_test inflate FILE OUT [WORKERS]` prints the bytes and their FNV-1a sum. On the card a
piece of 8 KB is some 160 thousand instructions, about 40 ms of one worker, which a small file
waits for whole: Half-Life leaves files under 32 KB as they are.

## One deflate stream on many cores

A deflate stream someone else made (a `.gz`, a zip's entry) has no pieces, and
`media_inflate_stream` (`m_inflate.c`) shares it out anyway, as pugz and rapidgzip do:

    why = media_inflate_stream(in, in_size, out, room, &made, &used, &crc, workers);
    why = media_inflate_one(in, in_size, out, room, &made, &used, &crc, scratch);  /* one core: a small one */

- The compressed bytes are cut in parts of 32 KB, a core each, fewer than gzip's dynamic blocks
  are apart (some 68 KB packed in a tar of source): every block is then the first of some part.
  A part finds the first dynamic block header in its bits (a table of 13 bits for the type
  and counts, one of 12 for the code lengths' code to be whole, then the whole header, its
  symbols to the end of its block, and a good header after it), and inflates from there to
  the next part's block. With 64 KB parts every third or fourth part found no block and its
  neighbour made two: the busiest worker was 30.4 million instructions, now 21.9.
- What it cannot know yet, a copy from the 32 KB before the part, it writes as a mark (16-bit
  values: a byte, or 0x8000 and the place in the window before). Marks do not die out in text:
  a part's last window still held 0 to 27,937 of them (a median of about 7,000), so the parts
  are inflated twice. The first core walks the chain once, part after part, each part's last
  32 KB made bytes from the window before it, and posts each part's second inflate, into its
  place with its window known, to that part's worker as soon as its window is known.
- The CRC-32 is sliced by eight on each part and the parts' sums joined (`media_crc32_join`);
  the texels two parts share are put back as each part made them.
- Fixed and stored blocks are only followed, never looked for: a stream of those is one part.

What it costs, `rvc_cpu`, 255 workers, the busybox source as a 2,121,744-byte `.tar.gz`
(8,816,640 bytes of tar) and the same files as a 2,673,622-byte zip (`unarc`, `docs/system/open.md`):

| | busybox | `unarc` |
|---|---|---|
| `.tar.gz` unpacked into 2,116 files (`tar xzf`, `unarc tgz`) | 831.7 M on core 0, 252 s | 87.3 M, 30.8 s |
| the zip (`unzip -q`, `unarc zip`) | 683.5 M, 207 s | 60.4 M, 20.1 s |
| the `.gz` alone into one file (`unarc gz`) | | 35.0 M, 15.2 s |

Of the `.gz`'s 15 s: reading the file 1 s, the parts' search and first inflate 4.5 s, the chain
on the first core with the second inflates beside it 4 s, writing 8.8 MB into `/tmp` about a
pass a page (1.5 s; `cat` of the same bytes is 2.2 s). Only 37 cores have a block to start
from: deflate's blocks are what the stream can be cut at. On one core the same stream is 224 M
(`symbols`, the decoding loop, some 15 instructions a byte; the CRC 5.5).
`media_test gunzip FILE [WORKERS [OUT]]` prints the bytes, the CRC and the FNV-1a sum.

## On the card

The shader machine on an RTX 5090 (`rvc_harness`, D3D11 + fxc2, through `tools\gpu_queue.ps1`;
its first core some 4.7 million instructions a second), 15 October 2026, real seconds, 63
workers, `media_test` as above (the pixels mapped before, the second of two decodings):

| | Card | `rvc_cpu`'s rounds |
|---|---|---|
| JPEG 1623 x 1080 in restart intervals | 0.57 to 0.69 s | 0.97 s |
| the same without markers (guessed parts) | 1.17 to 1.40 s | 1.76 s |
| the same, one core's reading before (`MEDIA_JPEG_SERIAL=1`) | 2.40 s | 4.12 s |
| the same, progressive | 5.88 s | 9.96 s |
| PNG 1623 x 1080 in segments | 1.33 s | 2.45 s |
| PNG in one stream (Pillow's; a sum of the pixels on the first core in it) | 16.1 s | |
| the JPEG in intervals on one core (the same sum in it) | 26.9 s | 44.6 s |
| JPEG encoding at quality 85 | 1.52 s | 2.3 s |
| PNG encoding at level 1 (32 workers) | 8.39 s | 16.3 s |
| GIF encoding, fixed colours | 1.35 s | 1.98 s |
| GIF animation, 12 frames of 320 x 240 | 3.96 s | 6.54 s |
| MJPEG AVI, 96 pictures of 320 x 176 (`MEDIA_TEST_NOSUM=1`) | 2.20 s: 44 a second | 3.44 s |
| MPEG-1, 480 pictures of 320 x 176 | 10.4 s: 46 a second | |
| `nxvideo` playing the AVI | none left out | none left out |
| the desktop's picture at its start, 800 x 600 (`UI_PICTURES_PRINT=1 nxfiles --desktop`) | 424 ms | 763 ms |

## What the image ships

| | Before | Now |
|---|---|---|
| the desktop's pictures (`tools/make_wallpaper.py`; `nanox.md`) | 11 PPM, 22 MB | JPEG in restart intervals, with twins for 800 x 600, 1280 x 720 and 1920 x 1080: 4.5 MB |
| ClassiCube's texture pack (`classicube.md`) | 2.99 MB of the game's pixel words | 104 KB of PNG in segments |
| the browser's cache of pictures (`web.md`, `fetch.md`) | 201 PPM, 7.3 MB | the same pictures, 24 JPEG, 7 PNG, 170 small PPM: 1.27 MB |
| the sample pictures (`tools/make_media.py`) | baseline JPEG | JPEG in restart intervals |
| Quake III's pictures (`quake3.md`) | texels in `quake3-se.dat`, 13.4 MB | 13 JPEG and 6 PNG sheets: 8.0 MB |
| the sample music | MP3 and FLAC (no Vorbis decoder) | two of the tunes Ogg Vorbis |

## Not done

- JPEG: arithmetic coding, 12-bit samples, lossless. A JPEG without markers takes 1.6 times as
  long as in restart intervals (three readings more); a progressive one waits for one core's pass
  through its scans. A colour sample at half size is the nearest one's, not smoothed (the browser
  keeps its JPEGs at 4:4:4 for that).
- PNG: APNG, and encoding is some 430 instructions a pixel (above).
- WebP; AVIF. GIF encoding: above 256 colours the fixed ones dithered, not a palette of the
  picture's own; an animation's frames one after another (each over every core), not side by
  side, and each the whole canvas (no rectangle of what changed).
- Most times above are `rvc_cpu`'s (rounds of 8,192 instructions a core); the card's are in
  "On the card", and the frames were captured with `rvc_harness --cpu --warp`.